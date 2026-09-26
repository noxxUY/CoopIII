// The CoopIII server, with no interface attached.
//
// Relays player/vehicle state, and relays the host player's time of day and
// weather to everyone else. Authority model is in docs/protocol.md §2.1, and
// §2.7 for why the clock belongs to a player rather than to this process.
//
// server.exe has two front ends over this one class - the window, and the
// console behind --nogui - in the same process either way, so the two cannot
// disagree about what a session is doing. The only thing a front end supplies
// is where the log lines go.
#pragma once

#include "cutscenevote.h"
#include "objectrecords.h"
#include "rampagevote.h"
#include "lobby.h"
#include "session.h"
#include "stuntcam.h"

#include "coopiii/net.h"
#include "coopiii/version.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace coopiii {

inline const char *RejectText(RejectReason reason) {
	switch (reason) {
	case REJECT_NONE:        return "accepted";
	case REJECT_BAD_VERSION: return "bad version";
	case REJECT_FULL:        return "the session is full";
	case REJECT_BAD_PASSWORD: return "wrong password, or none";
	}
	return "unknown";
}

inline uint32_t NowMs() {
	using namespace std::chrono;
	static const auto start = steady_clock::now();
	return static_cast<uint32_t>(
	    duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline void CopyText(char *dst, size_t capacity, const std::string &src) {
	std::memset(dst, 0, capacity);
	std::memcpy(dst, src.data(), std::min(capacity - 1, src.size()));
}

// What a log line is about, so the window can colour its tag column and
// filter the console the way design/screens/Server.dc.html does.
enum class LogKind : uint8_t {
	Info,     // the server talking about itself
	Join,     // somebody arrived
	Leave,    // somebody left, or was thrown out
	Detail,   // a vehicle changed hands, a ped moved, a player died
	Warn,     // a refusal, a dropped snapshot, a bind that failed
	Chat,     // a player said something
};

inline const char *TagOf(LogKind kind) { return kind == LogKind::Chat ? "[chat]" : "[coopiii]"; }

// Connections the server takes: every slot, a launcher waiting in the lobby
// for each (docs/protocol.md 1.31), and two to say "full" on.
constexpr uint8_t  SERVER_PEERS  = MAX_PLAYERS + LOBBY_MAX + 2;
// How long a connection has to say hello before it is dropped.
constexpr uint32_t HELLO_WAIT_MS = 10000;

class Server {
public:
	// ammoSync and wantedRule are defaulted rather than required so the two
	// front ends that already call this keep compiling; both pass both.
	bool Start(uint16_t port, bool friendlyFire, bool ammoSync = false,
	           uint8_t wantedRule  = WANTED_RULE_PERPLAYER,
	           uint8_t rampageRule = RAMPAGE_RULE_SHARED,
	           uint8_t cheatRule   = CHEAT_RULE_SHARED,
	           uint8_t moneyRule   = MONEY_RULE_OFF,
	           uint8_t packageRule = PACKAGES_SHARED) {
		if (!NetInit()) {
			Log(LogKind::Warn, "enet init failed");
			return false;
		}
		// Room past the last slot, so a ninth player is answered "full"
		// instead of never hearing back.
		if (!m_net.Listen(port, SERVER_PEERS)) {
			Log(LogKind::Warn, "cannot bind port %u", port);
			NetDeinit();
			return false;
		}
		m_session.SetFriendlyFire(friendlyFire);
		m_session.SetAmmoSync(ammoSync);
		m_session.SetWantedRule(wantedRule);
		m_session.SetRampageRule(rampageRule);
		m_session.SetCheatRule(cheatRule);
		m_session.SetMoneyRule(moneyRule);
		m_session.SetPackageRule(packageRule);
		m_port      = port;
		m_listening = true;
		m_startedMs = NowMs();
		Log(LogKind::Info, "CoopIII " COOPIII_VERSION " (protocol %u)",
		    static_cast<unsigned>(PROTOCOL_VERSION));
		Log(LogKind::Info, "listening on %u, %u slots, friendly fire %s, "
		            "ammo sync %s, wanted level %s, money %s, hidden packages %s",
		            port, MAX_PLAYERS,
		            friendlyFire ? "on" : "off", ammoSync ? "on" : "off",
		            m_session.WantedRule() == WANTED_RULE_SHARED ? "shared"
		                : m_session.WantedRule() == WANTED_RULE_OFF ? "off"
		                                                            : "per player",
		            m_session.MoneyRuleValue() == MONEY_RULE_SHARED ? "shared"
		                : m_session.MoneyRuleValue() == MONEY_RULE_OWN ? "own"
		                                                               : "off",
		            m_session.PackageRule() == PACKAGES_PERPLAYER ? "per player"
		                                                          : "shared");
		return true;
	}

	void Stop() {
		m_listening = false;
		m_net.Shutdown();
		NetDeinit();
		m_pendingHellos.clear();
		// Started again, it is a new session; Start puts every rule back.
		// Kept, the old players held the peer slots the next connections are
		// given again, so a hello from somebody reconnecting found the stale
		// record, was dropped as a duplicate and never welcomed, and all they
		// sent went to the ghost.
		m_session     = Session{};
		m_vote        = RampageVote{};
		m_cutscenes   = CutsceneVotes{};
		m_lobby       = Lobby{};
		m_events.clear();
		for (uint32_t &at : m_connectedAtMs)
			at = 0;
		m_lastTickMs  = 0;
		m_lastWorldMs = 0;
		m_lastPingsMs = 0;
		m_skyHolder   = INVALID_PLAYER;
	}

	// What a hello has to be followed by, or empty for nothing. protocol.h,
	// C_Password. Takes effect for the next hello; nobody already in is asked.
	// The server's missionFailOnDeath, missionMargin, missionEnemies and
	// missionScale (config.h).
	void SetMissionRules(bool failOnDeath, uint16_t marginCm,
	                     uint8_t enemies = MISSION_ENEMIES_ORIGINAL,
	                     uint16_t scalePct = MISSION_SCALE_DEFAULT) {
		m_session.Mission().SetFailOnDeath(failOnDeath);
		m_session.Mission().SetMarginCm(marginCm);
		m_session.Mission().SetEnemies(enemies, scalePct);
		Log(LogKind::Info,
		    "missions: everybody has to be within %.1f m of a start or a checkpoint, and a "
		    "death %s", MarginMetres(marginCm), failOnDeath ? "fails the mission" : "does not");
		if (enemies != MISSION_ENEMIES_ORIGINAL)
			Log(LogKind::Info, "missions: every player after the first makes the enemies %u%% "
			    "tougher", static_cast<unsigned>(scalePct));
	}
	// How long a start waits for a game in its own intro. MISSION_BUSY_WAIT_MS
	// but in a test.
	void SetMissionBusyWaitMs(uint32_t ms) { m_session.Mission().SetBusyWaitMs(ms); }

	void SetPassword(const std::string &password) {
		m_password = password;
		Log(LogKind::Info, m_password.empty() ? "no password: anybody with the address "
		                                        "can join"
		                                      : "players need the password to join");
	}

	// `waitMs` is how long Service may block waiting for the first event. The
	// console server can afford to wait; the window cannot, because it is also
	// drawing, so it passes 0 and comes back next frame.
	void Tick(uint32_t waitMs = 10) {
		const uint32_t now = NowMs();
		const uint32_t dt  = now - m_lastTickMs;
		m_lastTickMs       = now;

		m_session.Clock().Advance(dt);
		// Let go of pickups whose respawn window has passed, so the next
		// player to walk over one is granted it rather than denied.
		m_session.ExpirePickups(now);
		// And let go of wrecked unowned cars old enough that every machine's
		// engine has had time to clear the shell and let the generator park a
		// new car there. docs/roadmap.md 5.8 and WreckedUnownedCar.
		m_session.ExpireUnownedWrecks(now);
		// And the broken street objects nobody is near any more.
		ExpireObjectRecords(now);
		// And let go of session cars nobody has been in or near for a minute,
		// on every machine. VEHICLE_RELEASE_MS in session.h has the rule.
		ReleaseIdleVehicles(now);
		// And end a rampage nobody is left to end. Normally a client's own
		// CDarkel::Update times it out first and this never fires; it is what
		// answers the player who started a rampage and then disconnected, and
		// the session where everyone is sitting in the pause menu with CTimer
		// stopped. docs/roadmap.md 5.10 and Session::ExpireRampage.
		{
			RampageEndBody ended{};
			if (m_session.ExpireRampage(now, ended)) {
				Log(LogKind::Info,
				    "rampage %u failed on the session's own clock - nobody was "
				    "left running one to say so", ended.frenzyId);
				BroadcastRampageEnd(ended);
			}
		}
		// And the vote before one, which runs out on this clock.
		TickRampageVote(now);
		// And a skipped mission scene's grace for a participant late into it.
		FlushCutsceneVotes(now);

		// A mission's claim nobody refreshed: its player walked out of the
		// marker, and whoever was waited for is not any more.
		m_session.Mission().Expire(now);
		BroadcastMissionWaiting();

		m_events.clear();
		m_net.Service(m_events, waitMs);
		for (const ServerEvent &ev : m_events)
			Handle(ev);

		// A hello whose password never came. An older client, or one with
		// nothing in its ini. On a clock read after the service above, which
		// is where these were stamped: `now` is from before it.
		const uint32_t heldUntil = NowMs();
		for (size_t i = 0; i < m_pendingHellos.size();) {
			if (static_cast<int32_t>(heldUntil - m_pendingHellos[i].atMs) <
			    static_cast<int32_t>(PASSWORD_WAIT_MS)) {
				++i;
				continue;
			}
			const PendingHello late = m_pendingHellos[i];
			m_pendingHellos.erase(m_pendingHellos.begin() + static_cast<std::ptrdiff_t>(i));
			RejectHello(late.peer, late.hello, REJECT_BAD_PASSWORD, "no password came");
		}

		// A connection that never said who it is. Left, eight of them held
		// every slot and nobody else could get in.
		for (PeerId peer = 0; peer < SERVER_PEERS; ++peer) {
			const uint32_t at = m_connectedAtMs[peer];
			if (at == 0 || static_cast<int32_t>(heldUntil - at) < static_cast<int32_t>(HELLO_WAIT_MS))
				continue;
			m_connectedAtMs[peer] = 0;
			if (m_session.FindByPeer(peer) || m_lobby.Has(peer))
				continue;
			bool pending = false;
			for (const PendingHello &h : m_pendingHellos)
				pending = pending || h.peer == peer;
			if (pending)
				continue;
			m_net.Disconnect(peer, LEAVE_TIMEOUT);
			Log(LogKind::Warn, "dropped a connection that never said hello");
		}

		// World state at 1 Hz (docs/protocol.md §2.3). OnWorldState resets
		// this timer, so with a host reporting normally the tick below never
		// fires - it's what keeps the session's clock moving while the host
		// is on a loading screen or hasn't sent anything yet.
		if (now - m_lastPingsMs >= 1000) {
			m_lastPingsMs = now;
			BroadcastPings(now);
		}
		if (now - m_lastWorldMs >= 1000) {
			m_lastWorldMs = now;
			BroadcastWorldState();
		}
	}

private:
	void Handle(const ServerEvent &ev) {
		switch (ev.type) {
		case ServerEvent::CONNECT:
			// Nothing to do yet, wait for C_HELLO to say who this is - for a
			// while (HELLO_WAIT_MS).
			if (ev.peer < SERVER_PEERS)
				m_connectedAtMs[ev.peer] = NowMs() | 1u;
			break;
		case ServerEvent::DISCONNECT:
			if (ev.peer < SERVER_PEERS)
				m_connectedAtMs[ev.peer] = 0;
			OnDisconnect(ev.peer);
			break;
		case ServerEvent::MESSAGE:
			OnMessage(ev.peer, ev.msg);
			break;
		}
	}

	// Everybody, the claimer included. On the machine whose engine made the
	// car the client only forgets the row and leaves the car to its engine
	// (RemoteVehicle::ours); everywhere else the copy is destroyed. This is
	// one of three senders of S_VehicleDespawn, with ReleaseMissionCars and
	// OnVehicleRemoved.
	void ReleaseIdleVehicles(uint32_t now) {
		for (uint16_t netId : m_session.ReleaseIdleVehicles(now)) {
			S_VehicleDespawn out;
			InitHeader(out, now);
			out.netId = netId;
			m_net.Broadcast(out, CH_EVENT);
			Log(LogKind::Detail,
			    "vehicle %u released - nobody has been in it or within %.0f m "
			    "of it for %u s (%u session cars left)",
			    netId, static_cast<double>(VEHICLE_KEEP_RADIUS_M),
			    static_cast<unsigned>(VEHICLE_RELEASE_MS / 1000),
			    static_cast<unsigned>(m_session.LiveVehicleCount()));
		}
	}

	// The session's mission is over, however it ended: the cars it made that
	// nobody is in go from every machine, the same way an idle one does.
	// Nothing while a mission runs, whose cars are still its own.
	void ReleaseMissionCars(uint32_t now) {
		for (uint16_t netId : m_session.ReleaseMissionCars()) {
			S_VehicleDespawn out;
			InitHeader(out, now);
			out.netId = netId;
			m_net.Broadcast(out, CH_EVENT);
			Log(LogKind::Detail, "vehicle %u released - the mission that made it is over and "
			    "nobody is in it", netId);
		}
	}

	void OnDisconnect(PeerId peer) {
		for (size_t i = 0; i < m_pendingHellos.size(); ++i)
			if (m_pendingHellos[i].peer == peer) {
				m_pendingHellos.erase(m_pendingHellos.begin() + static_cast<std::ptrdiff_t>(i));
				break;
			}
		if (const Lobby::Member *m = m_lobby.FindByPeer(peer)) {
			Log(LogKind::Info, "%s left the lobby", m->nick.c_str());
			m_lobby.Leave(peer);
			BroadcastLobby();
			return;
		}
		RemovePlayer(peer, LEAVE_QUIT);
	}

	// Everything a player leaves behind, whether they went or were thrown
	// out. A kick used to do half of this: their reservations stayed held,
	// their traffic stayed as rows owned by an empty slot, frozen on every
	// screen and inherited by the next player in it, and the car they were in
	// was never handed on.
	void RemovePlayer(PeerId peer, uint8_t reason) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		if (reason == LEAVE_KICKED)
			Log(LogKind::Leave, "%s was kicked (slot %u)", p->nick.c_str(), p->id);
		else
			Log(LogKind::Leave, "%s left (slot %u)", p->nick.c_str(), p->id);

		S_PlayerLeave out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.reason   = reason;

		// Anything they had reserved is free again at once. What they
		// actually collected stays collected: the respawn window runs on
		// whether or not they are still connected.
		m_session.ReleaseReservationsOf(p->id);

		const uint8_t wasHost = m_session.HostId();
		// Before RemovePeer, while the ped rows still say whose they are and
		// the leaver is still here to be left out of who takes them.
		HandOverAmbientOf(*p);
		// And the car they were in, to somebody standing next to it. After the
		// leave on the wire, so every machine has taken their ped out of the
		// seat before it hears who settles the car.
		const std::vector<uint16_t> handed = m_session.HandOverVehiclesOf(p->id);
		const std::string           nick   = p->nick;
		const bool     ownedMission = m_session.Mission().Running() &&
		                              m_session.Mission().Owner() == out.playerId;
		const uint16_t mission      = m_session.Mission().Number();
		const bool     missionMoved = m_session.Mission().Leave(out.playerId);
		m_session.RemovePeer(peer);
		m_net.SetMember(peer, false);
		m_net.Broadcast(out, CH_EVENT, peer);
		// A vote they started is off; one they were voting in is recounted
		// without them.
		TickRampageVote(NowMs());
		// And so is the cutscene they were in.
		m_cutscenes.Leave(out.playerId);
		FlushCutsceneVotes(NowMs());
		// After the leave, so every machine has them gone before it hears the
		// mission end or lose a participant.
		if (missionMoved) {
			if (ownedMission)
				Log(LogKind::Info, "%s left in the middle of %s, which fails for everybody",
				    nick.c_str(), MissionName(mission));
			BroadcastMissionState();
			ReleaseMissionCars(NowMs());
		}
		BroadcastMissionWaiting();
		for (uint16_t netId : handed) {
			AnnounceCustody(netId, NowMs());
			const Player *heir = m_session.FindById(m_session.CustodianOf(netId));
			Log(LogKind::Detail, "%s's car %u is %s's to settle now", nick.c_str(), netId,
			    heir ? heir->nick.c_str() : "?");
		}

		// The host walking out hands the clock to whoever is left, unless a
		// mission runs, whose owner keeps it. Everyone finds out from the next
		// world packet, at most a second away, which is also when the new
		// holder starts reporting.
		if (wasHost == out.playerId && m_session.HostId() != INVALID_PLAYER) {
			const Player *host = m_session.FindById(m_session.HostId());
			Log(LogKind::Info, "the host left; %s is the host now",
			            host ? host->nick.c_str() : "?");
		}
		NoteSkyHolder();
		// The lobby sees who is playing.
		BroadcastLobby();
	}

	void OnMessage(PeerId peer, const Message &msg) {
		switch (msg.opcode) {
		case OP_C_HELLO:
			if (const auto *pkt = msg.as<C_Hello>())
				OnHello(peer, *pkt);
			break;
		case OP_C_PASSWORD:
			if (const auto *pkt = msg.as<C_Password>())
				OnPassword(peer, *pkt);
			break;
		case OP_C_PLAYER_STATE:
			if (const auto *pkt = msg.as<C_PlayerState>())
				OnPlayerState(peer, *pkt);
			break;
		case OP_C_PLAYER_STATE_RIDE:
			if (const auto *pkt = msg.as<C_PlayerStateRide>())
				OnPlayerStateRide(peer, *pkt);
			break;
		case OP_C_VEHICLE_STATE:
			if (const auto *pkt = msg.as<C_VehicleState>())
				OnVehicleState(peer, *pkt);
			break;
		case OP_C_PLAYER_MODEL:
			if (const auto *pkt = msg.as<C_PlayerModel>())
				OnPlayerModel(peer, *pkt);
			break;
		case OP_C_PLAYER_LOOK:
			if (const auto *pkt = msg.as<C_PlayerLook>())
				OnPlayerLook(peer, *pkt);
			break;
		case OP_C_PLAYER_AWAY:
			if (const auto *pkt = msg.as<C_PlayerAway>())
				OnPlayerAway(peer, *pkt);
			break;
		case OP_C_PLAYER_AMMO:
			if (const auto *pkt = msg.as<C_PlayerAmmo>())
				OnPlayerAmmo(peer, *pkt);
			break;
		case OP_C_ENTER_VEHICLE:
			if (const auto *pkt = msg.as<C_EnterVehicle>())
				OnEnterVehicle(peer, *pkt);
			break;
		case OP_C_ENTERING_VEHICLE:
			if (const auto *pkt = msg.as<C_EnteringVehicle>())
				OnEnteringVehicle(peer, *pkt);
			break;
		case OP_C_JACKING_VEHICLE:
			if (const auto *pkt = msg.as<C_JackingVehicle>())
				OnJackingVehicle(peer, *pkt);
			break;
		case OP_C_SHOT:
			if (const auto *pkt = msg.as<C_Shot>())
				OnShot(peer, *pkt);
			break;
		case OP_C_EXPLOSION:
			if (const auto *pkt = msg.as<C_Explosion>())
				OnExplosion(peer, *pkt);
			break;
		case OP_C_MINE_BLAST:
			if (const auto *pkt = msg.as<C_MineBlast>())
				OnMineBlast(peer, *pkt);
			break;
		case OP_C_MISSION_BOMB:
			if (const auto *pkt = msg.as<C_MissionBomb>())
				OnMissionBomb(peer, *pkt);
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
		case OP_C_VEHICLE_REMOVED:
			if (const auto *pkt = msg.as<C_VehicleRemoved>())
				OnVehicleRemoved(peer, *pkt);
			break;
		case OP_C_VEHICLE_SETTLED:
			if (const auto *pkt = msg.as<C_VehicleSettled>())
				OnVehicleSettled(peer, *pkt);
			break;
		case OP_C_VEHICLE_BLOWUP:
			if (const auto *pkt = msg.as<C_VehicleBlowUp>())
				OnVehicleBlowUp(peer, *pkt);
			break;
		case OP_C_UNOWNED_BLOWUP:
			if (const auto *pkt = msg.as<C_UnownedBlowUp>())
				OnUnownedBlowUp(peer, *pkt);
			break;
		case OP_C_VEHICLE_DAMAGE:
			if (const auto *pkt = msg.as<C_VehicleDamage>())
				OnVehicleDamage(peer, *pkt);
			break;
		case OP_C_VEHICLE_BOMB:
			if (const auto *pkt = msg.as<C_VehicleBomb>())
				OnVehicleBomb(peer, *pkt);
			break;
		case OP_C_VEHICLE_RADIO:
			if (const auto *pkt = msg.as<C_VehicleRadio>())
				OnVehicleRadio(peer, *pkt);
			break;
		case OP_C_VEHICLE_ALARM:
			if (const auto *pkt = msg.as<C_VehicleAlarm>())
				OnVehicleAlarm(peer, *pkt);
			break;
		case OP_C_VEHICLE_AIM:
			if (const auto *pkt = msg.as<C_VehicleAim>())
				OnVehicleAim(peer, *pkt);
			break;
		case OP_C_VEHICLE_HIT:
			if (const auto *pkt = msg.as<C_VehicleHit>())
				OnVehicleHit(peer, *pkt);
			break;
		case OP_C_CAR_HIT:
			if (const auto *pkt = msg.as<C_CarHit>())
				OnCarHit(peer, *pkt);
			break;
		case OP_C_WORLD_STATE:
			if (const auto *pkt = msg.as<C_WorldState>())
				OnWorldState(peer, *pkt);
			break;
		case OP_C_CHAT:
			if (const auto *pkt = msg.as<C_Chat>())
				OnChat(peer, *pkt);
			break;
		case OP_C_KICK:
			if (const auto *pkt = msg.as<C_Kick>())
				OnKick(peer, *pkt);
			break;
		case OP_C_LOBBY_JOIN:
			if (const auto *pkt = msg.as<C_LobbyJoin>())
				OnLobbyJoin(peer, *pkt);
			break;
		case OP_C_LOBBY_START:
			if (const auto *pkt = msg.as<C_LobbyStart>())
				OnLobbyStart(peer, *pkt);
			break;
		case OP_C_MISSION_CLAIM:
			if (const auto *pkt = msg.as<C_MissionClaim>())
				OnMissionClaim(peer, *pkt);
			break;
		case OP_C_MISSION_STARTED:
			if (const auto *pkt = msg.as<C_MissionStarted>())
				OnMissionStarted(peer, *pkt);
			break;
		case OP_C_MISSION_ENDED:
			if (const auto *pkt = msg.as<C_MissionEnded>())
				OnMissionEnded(peer, *pkt);
			break;
		case OP_C_MISSION_CHECKPOINT:
			if (const auto *pkt = msg.as<C_MissionCheckpoint>())
				OnMissionCheckpoint(peer, *pkt);
			break;
		case OP_C_MISSION_EFFECT:
			if (const auto *pkt = msg.as<C_MissionEffect>())
				OnMissionEffect(peer, *pkt);
			break;
		case OP_C_MISSION_WIDGET:
			if (const auto *pkt = msg.as<C_MissionWidget>())
				OnMissionWidget(peer, *pkt);
			break;
		case OP_C_MISSION_READY:
			if (const auto *pkt = msg.as<C_MissionReady>())
				OnMissionReady(peer, *pkt);
			break;
		case OP_C_MISSION_SEATS:
			if (const auto *pkt = msg.as<C_MissionSeats>())
				OnMissionSeats(peer, *pkt);
			break;
		case OP_C_MISSION_BOARD:
			if (const auto *pkt = msg.as<C_MissionBoard>())
				OnMissionBoard(peer, *pkt);
			break;
		case OP_C_MISSION_OBJECT_BREAK:
			if (const auto *pkt = msg.as<C_MissionObjectBreak>())
				OnMissionObjectBreak(peer, *pkt);
			break;
		case OP_C_MISSION_PICKUP:
			if (const auto *pkt = msg.as<C_MissionPickup>())
				OnMissionPickup(peer, *pkt);
			break;
		case OP_C_MISSION_KILL:
			if (const auto *pkt = msg.as<C_MissionKill>())
				OnMissionKill(peer, *pkt);
			break;
		case OP_C_MISSION_ANSWERS:
			if (const auto *pkt = msg.as<C_MissionAnswers>())
				OnMissionAnswers(peer, *pkt);
			break;
		case OP_C_MISSION_BUSY:
			if (const auto *pkt = msg.as<C_MissionBusy>())
				OnMissionBusy(peer, *pkt);
			break;
		case OP_C_MISSION_CATCH_UP:
			if (const auto *pkt = msg.as<C_MissionCatchUp>())
				OnMissionCatchUp(peer, *pkt);
			break;
		case OP_C_CAMPAIGN_DELTA:
			if (const auto *pkt = msg.as<C_CampaignDelta>())
				OnCampaignDelta(peer, *pkt);
			break;
		case OP_C_CAMPAIGN_SINCE:
			if (const auto *pkt = msg.as<C_CampaignSince>())
				OnCampaignSince(peer, *pkt);
			break;
		case OP_C_DESYNC_PROBE:
			if (const auto *pkt = msg.as<C_DesyncProbe>())
				OnDesyncProbe(peer, *pkt);
			break;
		case OP_C_PED_SPAWN:
			if (const auto *pkt = msg.as<C_PedSpawn>())
				OnPedSpawn(peer, *pkt);
			break;
		case OP_C_PED_DESPAWN:
			if (const auto *pkt = msg.as<C_PedDespawn>())
				OnPedDespawn(peer, *pkt);
			// This `break` was missing, so a ped despawn fell through into
			// the pickup claim below. Harmless only by accident - as<> checks
			// the opcode as well as the size, so the claim never matched -
			// but the next case added under it would not have been so lucky.
			break;
		case OP_C_PED_BODY_PART:
			if (const auto *pkt = msg.as<C_PedBodyPart>())
				OnPedBodyPart(peer, *pkt);
			break;
		case OP_C_PED_DEATH:
			if (const auto *pkt = msg.as<C_PedDeath>())
				OnPedDeath(peer, *pkt);
			break;
		case OP_C_PED_DAMAGE:
			if (const auto *pkt = msg.as<C_PedDamage>())
				OnPedDamage(peer, *pkt);
			break;
		case OP_C_NPC_SHOT:
			if (const auto *pkt = msg.as<C_NpcShot>())
				OnNpcShot(peer, *pkt);
			break;
		case OP_C_NPC_DAMAGE:
			if (const auto *pkt = msg.as<C_NpcDamage>())
				OnNpcDamage(peer, *pkt);
			break;
		case OP_C_NPC_VEHICLE_HIT:
			if (const auto *pkt = msg.as<C_NpcVehicleHit>())
				OnNpcVehicleHit(peer, *pkt);
			break;
		case OP_C_CAR_SPAWN:
			if (const auto *pkt = msg.as<C_CarSpawn>())
				OnCarSpawn(peer, *pkt);
			break;
		case OP_C_CAR_DESPAWN:
			if (const auto *pkt = msg.as<C_CarDespawn>())
				OnCarDespawn(peer, *pkt);
			break;
		case OP_C_CAR_LET_GO:
			if (const auto *pkt = msg.as<C_CarLetGo>())
				OnCarLetGo(peer, *pkt);
			break;
		case OP_C_CAR_STATES:
			if (const auto *pkt = msg.as<C_CarStates>())
				OnCarStates(peer, *pkt);
			break;
		case OP_C_PED_STATES:
			if (const auto *pkt = msg.as<C_PedStates>())
				OnPedStates(peer, *pkt);
			break;
		case OP_C_PICKUP_CLAIM:
			if (const auto *pkt = msg.as<C_PickupClaim>())
				OnPickupClaim(peer, *pkt);
			break;
		case OP_C_PICKUP_RELEASE:
			if (const auto *pkt = msg.as<C_PickupRelease>())
				OnPickupRelease(peer, *pkt);
			break;
		case OP_C_PICKUP_COLLECTED:
			if (const auto *pkt = msg.as<C_PickupCollected>())
				OnPickupCollected(peer, *pkt);
			break;
		case OP_C_PICKUP_DROP:
			if (const auto *pkt = msg.as<C_PickupDrop>())
				OnPickupDrop(peer, *pkt);
			break;
		case OP_C_GARAGE_STATE:
			if (const auto *pkt = msg.as<C_GarageState>())
				OnGarageState(peer, *pkt);
			break;
		case OP_C_RESPRAY:
			if (const auto *pkt = msg.as<C_Respray>())
				OnRespray(peer, *pkt);
			break;
		case OP_C_OBJECT_BROKEN:
			if (const auto *pkt = msg.as<C_ObjectBroken>())
				OnObjectBroken(peer, *pkt);
			break;
		case OP_C_RAMPAGE_START:
			if (const auto *pkt = msg.as<C_RampageStart>())
				OnRampageStart(peer, *pkt);
			break;
		case OP_C_RAMPAGE_KILL:
			if (const auto *pkt = msg.as<C_RampageKill>())
				OnRampageKill(peer, *pkt);
			break;
		case OP_C_RAMPAGE_CAR:
			if (const auto *pkt = msg.as<C_RampageCar>())
				OnRampageCar(peer, *pkt);
			break;
		case OP_C_RAMPAGE_END:
			if (const auto *pkt = msg.as<C_RampageEnd>())
				OnRampageEnd(peer, *pkt);
			break;
		case OP_C_RAMPAGE_VOTE:
			if (const auto *pkt = msg.as<C_RampageVote>())
				OnRampageVote(peer, *pkt);
			break;
		case OP_C_RAMPAGE_ARRIVED:
			if (const auto *pkt = msg.as<C_RampageArrived>())
				OnRampageArrived(peer, *pkt);
			break;
		case OP_C_CUTSCENE_STATE:
			if (const auto *pkt = msg.as<C_CutsceneState>())
				OnCutsceneState(peer, *pkt);
			break;
		case OP_C_OBJECT_SETTLED:
			if (const auto *pkt = msg.as<C_ObjectSettled>())
				OnObjectSettled(peer, *pkt);
			break;
		case OP_C_OBJECT_REBUILT:
			if (const auto *pkt = msg.as<C_ObjectRebuilt>())
				OnObjectRebuilt(peer, *pkt);
			break;
		case OP_C_GATE_STATE:
			if (const auto *pkt = msg.as<C_GateState>())
				OnGateState(peer, *pkt);
			break;
		case OP_C_HELI_STATE:
			if (const auto *pkt = msg.as<C_HeliState>())
				OnHeliState(peer, *pkt);
			break;
		case OP_C_HELI_GONE:
			if (const auto *pkt = msg.as<C_HeliGone>())
				OnHeliGone(peer, *pkt);
			break;
		case OP_C_HELI_HIT:
			if (const auto *pkt = msg.as<C_HeliHit>())
				OnHeliHit(peer, *pkt);
			break;
		case OP_C_HELI_SHOT:
			if (const auto *pkt = msg.as<C_HeliShot>())
				OnHeliShot(peer, *pkt);
			break;
		case OP_C_CHEAT:
			if (const auto *pkt = msg.as<C_Cheat>())
				OnCheat(peer, *pkt);
			break;
		case OP_C_PED_REVIVE:
			if (const auto *pkt = msg.as<C_PedRevive>())
				OnPedRevive(peer, *pkt);
			break;
		case OP_C_WATER_CANNON:
			if (const auto *pkt = msg.as<C_WaterCannon>())
				OnWaterCannon(peer, *pkt);
			break;
		case OP_C_CAR_LISTS:
			if (const auto *pkt = msg.as<C_CarLists>())
				OnCarLists(peer, *pkt);
			break;
		case OP_C_STUNT_CAMERA:
			if (const auto *pkt = msg.as<C_StuntCamera>())
				OnStuntCamera(peer, *pkt);
			break;
		case OP_C_MONEY_CHANGE:
			if (const auto *pkt = msg.as<C_MoneyChange>())
				OnMoneyChange(peer, *pkt);
			break;
		case OP_C_MONEY_AWARD:
			if (const auto *pkt = msg.as<C_MoneyAward>())
				OnMoneyAward(peer, *pkt);
			break;
		default:
			// Unknown or not implemented, ignore it. Length is never trusted
			// blindly either way, Message::as<> already rejects mismatched sizes.
			break;
		}
	}

	// ---- ambient peds ------------------------------------------------------
	//
	// docs/population.md §1.2. The server's only job is to name the thing:
	// the ped already exists on the claimer's machine and is already running
	// its AI there. Nothing here decides whether it should exist.
	//
	// The broadcast includes the claimer, and that is the whole point of the
	// handshake - it is the only way they learn what their own ped is called.
	void OnPedSpawn(PeerId peer, const C_PedSpawn &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// A claim with no temporary id has nothing to answer to. Refusing it
		// is not pedantry: S_PedSpawn with tempId 0 is what the backfill
		// sends, so letting one through here would hand the claimer a packet
		// it is supposed to read as somebody else's ped.
		if (in.tempId == 0) {
			if (!p->warnedPedTempId) {
				p->warnedPedTempId = true;
				Log(LogKind::Warn, "%s claimed a ped with temp id 0; "
				            "ignoring (and not saying so again)",
				            p->nick.c_str());
			}
			return;
		}

		AmbientPed *ped = m_session.AddPed(p->id, in.body);
		if (!ped) {
			// Loudly, but once per player: the engine on the other end is a
			// pedestrian generator and it will keep trying forever.
			if (!p->warnedPedCap) {
				p->warnedPedCap = true;
				Log(LogKind::Warn, "the session is full of ambient peds "
				            "(%zu); refusing %s's claims from here on",
				            MAX_AMBIENT_PEDS, p->nick.c_str());
			}
			return;
		}

		S_PedSpawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.tempId        = in.tempId;
		out.netId         = ped->netId;
		out.body          = in.body;
		m_net.Broadcast(out, CH_EVENT);
	}

	void OnPedDespawn(PeerId peer, const C_PedDespawn &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// Session::RemovePed is the one that enforces ownership; a refusal
		// here means an observer tried to delete somebody else's ped, and it
		// gets nothing, not even a relay.
		if (!m_session.RemovePed(in.netId, p->id))
			return;

		S_PedDespawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.netId = in.netId;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A limb off one of the sender's own pedestrians. Relayed and not kept -
	// protocol.h, S_PedBodyPart says why nobody needs it later.
	//
	// Two refusals, both about what an observer's engine would be handed.
	// Only the ped's owner may say it, the same rule as a despawn, or one
	// machine could strip somebody else's pedestrians. And only the five
	// nodes CPed::InflictDamage ever passes: the observer indexes
	// CPed::m_pFrames with it, and anything else is a read off the end of
	// that array on every other machine in the session.
	void OnPedBodyPart(PeerId peer, const C_PedBodyPart &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// One of the sender's own pedestrians, or the sender's own player:
		// the one machine that shoots either of them for real.
		const AmbientPed *ped = m_session.FindPed(in.body.netId);
		if ((!ped || ped->ownerPlayerId != p->id) && in.body.netId != p->netId)
			return;
		if (!IsRemovableBodyPart(in.body.node))
			return;

		S_PedBodyPart out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.body = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// One of the sender's own pedestrians died. Relayed *and* kept, which is
	// the one way this differs from the limb beside it: a corpse lies in the
	// road for the minute CPopulation takes to reap it, so a joiner inside
	// that minute has to be told (see Session::NotePedDeath and the corpse
	// branch in BuildBackfill).
	//
	// Every refusal is Session::NotePedDeath's - a ped nobody has, somebody
	// else's ped, or a second death for one life. The relay does not
	// second-guess any of them, and nothing is broadcast for a claim the
	// session would not record: a death the server does not believe must not
	// reach an observer either, or the two would disagree about a pedestrian
	// for as long as he exists.
	void OnPedDeath(PeerId peer, const C_PedDeath &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.NotePedDeath(in.body, p->id))
			return;

		S_PedDeath out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.body = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A medic on the sender's machine stood a dead pedestrian up. To everybody
	// else, his host included, and only when the session agrees he was dead
	// (Session::NotePedRevive): a revive the server does not record must not
	// reach anybody either, or a joiner and the rest would disagree about him.
	void OnPedRevive(PeerId peer, const C_PedRevive &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NotePedRevive(in.body.netId))
			return;

		S_PedRevive out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// One frame of a fire truck's jet, from the machine that aims it
	// (Session::MaySprayCannon), to everybody else. Nothing is kept: a jet is
	// over a moment after it stops, so a joiner has nothing to be handed.
	void OnWaterCannon(PeerId peer, const C_WaterCannon &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.MaySprayCannon(p->id, in.body.netId))
			return;

		S_WaterCannon out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// A hit one player's machine landed on a pedestrian another player hosts.
	//
	// The mirror image of the three handlers above and the only ambient packet
	// that travels *towards* an owner. Every decision in it is
	// Session::PedDamageRecipient's, which is where the inverted ownership rule,
	// the corpse test and the reason friendly fire has no say are written down.
	//
	// Point to point, like OnDamage and for the same reason: only the owner has
	// anything to do with it. What the other machines need to see - the flinch,
	// the limb, the corpse - reaches them from the owner afterwards, on its own
	// ped stream and on C_PedBodyPart and C_PedDeath.
	void OnPedDamage(PeerId peer, const C_PedDamage &in) {
		const Player *attacker = m_session.FindByPeer(peer);
		if (!attacker)
			return;

		// Every refusal is Session::PedDamageRecipient's - a pedestrian nobody
		// has, a sender claiming a hit on its own, one already reported dead, an
		// owner who has gone - and the relay does not second-guess any of them,
		// the same division OnPedDeath has with NotePedDeath.
		const Player *owner = m_session.PedDamageRecipient(in.body.netId, attacker->id);
		if (!owner)
			return;

		// The weapon, the piece and the direction are NOT bounded here, and that
		// is the same position OnDamage takes about the identical three fields.
		// It is deliberate rather than an oversight: what counts as a cause a
		// shooter may decide, which pieces exist and how many hit directions
		// there are are facts about the engine, they live in
		// client/src/game/combat.h beside the disassembly that proves each one,
		// and the receiving client checks all three before it calls the engine.
		// A second copy of that list in here would be a second thing to keep in
		// step with a binary this process has never loaded.
		//
		// The contrast is OnPedBodyPart, which does bound its node - because
		// IsRemovableBodyPart is a *wire* fact and lives in protocol.h, where
		// both ends read the same one.

		S_PedDamage out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.attackerId = attacker->id;
		out.body       = in.body;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// A round one of the sender's own pedestrians fired, to everybody else to
	// be drawn. Unreliable on both legs: it decides nothing where it lands.
	void OnNpcShot(PeerId peer, const C_NpcShot &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NpcShotAllowed(in.pedNetId, p->id))
			return;

		S_NpcShot out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.pedNetId      = in.pedNetId;
		out.body          = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// A hit one of the sender's pedestrians landed on another player's copy,
	// to that player alone. Session::NpcHitRecipient is every refusal. The
	// weapon, the piece and the direction are left to the victim's client to
	// bound, for the reason OnPedDamage gives.
	void OnNpcDamage(PeerId peer, const C_NpcDamage &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const Player *victim =
		    m_session.NpcHitRecipient(in.attackerPedNetId, in.body.victimNetId, p->id);
		if (!victim)
			return;

		S_NpcDamage out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId    = p->id;
		out.attackerPedNetId = in.attackerPedNetId;
		out.body             = in.body;
		m_net.SendTo(victim->peer, out, CH_EVENT);
	}

	// The same, for a round that reached a car a player drives or settles. Its
	// driver or custodian, by C_VehicleHit's own rule, and never custody for a
	// car nobody holds: a pedestrian shooting a parked car hands it to nobody.
	void OnNpcVehicleHit(PeerId peer, const C_NpcVehicleHit &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NpcShotAllowed(in.attackerPedNetId, p->id))
			return;
		const Player *owner = m_session.VehicleHitRecipient(in.body.netId, p->id);
		if (!owner)
			return;

		S_NpcVehicleHit out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId    = p->id;
		out.attackerPedNetId = in.attackerPedNetId;
		out.body             = in.body;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// The ped stream, relayed exactly the way the traffic one is: rows the
	// sender does not own are taken out, and the packet is not thrown away
	// over one of them. A batch is twelve unrelated pedestrians, and one
	// stale netId in it - a ped the owner's engine reaped a frame ago, which
	// is the ordinary race, since the despawn is reliable and this is not -
	// must not silence the other eleven.
	void OnPedStates(PeerId peer, const C_PedStates &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_PedStates out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.count         = 0;

		const uint8_t n = in.count < MAX_PED_STATES ? in.count : MAX_PED_STATES;
		for (uint8_t i = 0; i < n; ++i) {
			// NotePedState is what enforces ownership, the same way
			// Session::RemovePed does for a despawn, and it keeps the
			// session's own copy current so a latecomer's backfill puts the
			// pedestrian where he is rather than where he was born.
			if (!m_session.NotePedState(in.peds[i], p->id))
				continue;
			if (AmbientPed *ped = m_session.FindPed(in.peds[i].netId))
				ped->history.Note(in.hdr.sendTimeMs, in.peds[i].pos, p->id);
			out.peds[out.count++] = in.peds[i];
		}

		if (out.count == 0)
			return;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// A leaver's crowd: what somebody is near enough to keep goes to the
	// nearest player, the rest goes with him. Session::HandOverAmbientOf has
	// already moved the rows; this tells everybody, the adopter included,
	// before the S_PlayerLeave. A car and its occupants always share a
	// verdict and a packet (server/core/adopt.h).
	void HandOverAmbientOf(const Player &leaver) {
		const uint32_t now = NowMs();
		const std::vector<AdoptVerdict> verdicts = m_session.HandOverAmbientOf(leaver.id);

		size_t goneP = 0, goneC = 0, keptP = 0, keptC = 0;
		size_t perP[MAX_PLAYERS] = {}, perC[MAX_PLAYERS] = {};
		for (const AdoptVerdict &v : verdicts) {
			const bool car = v.kind == AMBIENT_ADOPT_CAR;
			if (v.adopter != INVALID_PLAYER) {
				size_t *per = car ? perC : perP;
				if (car)
					++keptC;
				else
					++keptP;
				if (v.adopter < MAX_PLAYERS)
					++per[v.adopter];
				continue;
			}
			if (car) {
				++goneC;
				S_CarDespawn out;
				InitHeader(out, now);
				out.netId = v.netId;
				m_net.Broadcast(out, CH_EVENT);
			} else {
				++goneP;
				S_PedDespawn out;
				InitHeader(out, now);
				out.netId = v.netId;
				m_net.Broadcast(out, CH_EVENT);
			}
		}

		for (const std::vector<AmbientAdoptRow> &batch : PackAdoptRows(verdicts)) {
			S_AmbientAdopt out{};
			InitHeader(out, now);
			out.wasOwnerPlayerId = leaver.id;
			out.count            = static_cast<uint8_t>(batch.size());
			out.why              = AMBIENT_ADOPT_LEFT;
			for (size_t i = 0; i < batch.size(); ++i)
				out.rows[i] = batch[i];
			m_net.Broadcast(out, CH_EVENT);
		}

		if (verdicts.empty())
			return;
		Log(LogKind::Detail,
		    "%s's crowd: %zu ped(s) and %zu car(s) handed on, %zu ped(s) and %zu "
		    "car(s) nobody was near enough to keep", leaver.nick.c_str(), keptP, keptC,
		    goneP, goneC);
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
			if (perP[id] == 0 && perC[id] == 0)
				continue;
			const Player *heir = m_session.FindById(id);
			Log(LogKind::Detail, "  %s takes %zu ped(s) and %zu car(s)",
			    heir ? heir->nick.c_str() : "?", perP[id], perC[id]);
		}
	}

	// A traffic car its host's engine dropped by distance with somebody else
	// near it (protocol.h, C_CarLetGo). Session::LetGoCar has decided and moved
	// the rows. Everybody but the sender is told the way a leaver's crowd is
	// told; the sender, whose engine has already deleted its own car, is sent
	// what it now watches as backfill under the new owner, so its screen can
	// have a copy when it comes back into reach.
	void OnCarLetGo(PeerId peer, const C_CarLetGo &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const uint32_t now = NowMs();
		const size_t   n   = in.pedCount < MAX_LET_GO_PEDS ? in.pedCount : MAX_LET_GO_PEDS;
		const std::vector<AdoptVerdict> verdicts =
		    m_session.LetGoCar(in.netId, in.peds, n, p->id, now);

		uint8_t adopter = INVALID_PLAYER;
		size_t  keptP = 0, goneP = 0;
		bool    carKept = false, carGone = false;
		for (const AdoptVerdict &v : verdicts) {
			const bool car = v.kind == AMBIENT_ADOPT_CAR;
			if (v.adopter != INVALID_PLAYER) {
				adopter = v.adopter;
				if (car) {
					carKept = true;
					const AmbientCar *row = m_session.FindCar(v.netId);
					if (!row)
						continue;
					S_CarSpawn spawn;
					InitHeader(spawn, now);
					spawn.ownerPlayerId = v.adopter;
					spawn.tempId        = 0;
					spawn.netId         = v.netId;
					spawn.body          = row->body;
					m_net.SendTo(peer, spawn, CH_EVENT);
					if (row->damagePanels != 0 || row->damageDoors != 0) {
						S_VehicleDamage dmg{};
						InitHeader(dmg, now);
						dmg.playerId    = INVALID_PLAYER;
						dmg.body.netId  = v.netId;
						dmg.body.panels = row->damagePanels;
						dmg.body.doors  = row->damageDoors;
						m_net.SendTo(peer, dmg, CH_EVENT);
					}
				} else {
					++keptP;
					const AmbientPed *row = m_session.FindPed(v.netId);
					if (!row)
						continue;
					S_PedSpawn spawn;
					InitHeader(spawn, now);
					spawn.ownerPlayerId = v.adopter;
					spawn.tempId        = 0;
					spawn.netId         = v.netId;
					spawn.body          = row->body;
					m_net.SendTo(peer, spawn, CH_EVENT);
				}
				continue;
			}
			if (car) {
				carGone = true;
				S_CarDespawn out;
				InitHeader(out, now);
				out.netId = v.netId;
				m_net.Broadcast(out, CH_EVENT, peer);
			} else {
				++goneP;
				S_PedDespawn out;
				InitHeader(out, now);
				out.netId = v.netId;
				m_net.Broadcast(out, CH_EVENT, peer);
			}
		}

		for (const std::vector<AmbientAdoptRow> &batch : PackAdoptRows(verdicts)) {
			S_AmbientAdopt out{};
			InitHeader(out, now);
			out.wasOwnerPlayerId = p->id;
			out.count            = static_cast<uint8_t>(batch.size());
			out.why              = AMBIENT_ADOPT_LET_GO;
			for (size_t i = 0; i < batch.size(); ++i)
				out.rows[i] = batch[i];
			m_net.Broadcast(out, CH_EVENT, peer);
		}

		if (carKept) {
			const Player *heir = m_session.FindById(adopter);
			Log(LogKind::Detail,
			    "%s's engine let go of traffic car %u with somebody near it; %s hosts it "
			    "now, with %zu of its people",
			    p->nick.c_str(), in.netId, heir ? heir->nick.c_str() : "?", keptP);
		} else if (carGone) {
			Log(LogKind::Detail,
			    "%s's engine let go of traffic car %u and nobody who has not let go of it "
			    "lately is near enough to keep it; gone, with %zu of its people",
			    p->nick.c_str(), in.netId, goneP);
		}
	}

	// ---- ambient traffic ---------------------------------------------------
	//
	// docs/population.md §3 step 4. The ped handlers above with the names
	// changed, plus the state relay a ped has no equivalent of.

	void OnCarSpawn(PeerId peer, const C_CarSpawn &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		if (in.tempId == 0) {
			if (!p->warnedCarTempId) {
				p->warnedCarTempId = true;
				Log(LogKind::Warn, "%s claimed a car with temp id 0; "
				                   "ignoring (and not saying so again)",
				    p->nick.c_str());
			}
			return;
		}

		AmbientCar *car = m_session.AddCar(p->id, in.body);
		if (!car) {
			if (!p->warnedCarCap) {
				p->warnedCarCap = true;
				Log(LogKind::Warn, "the session is full of ambient cars "
				                   "(%zu); refusing %s's claims from here on",
				    MAX_AMBIENT_CARS, p->nick.c_str());
			}
			return;
		}

		S_CarSpawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.tempId        = in.tempId;
		out.netId         = car->netId;
		out.body          = in.body;
		m_net.Broadcast(out, CH_EVENT);
	}

	void OnCarDespawn(PeerId peer, const C_CarDespawn &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.RemoveCar(in.netId, p->id))
			return;

		S_CarDespawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.netId = in.netId;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// Relayed as a whole batch, with the rows the sender does not own taken
	// out rather than the packet thrown away.
	//
	// Dropping the packet would be the harsher rule and the wrong one here: a
	// batch is eight unrelated cars, and one stale netId in it - a car the
	// owner despawned a frame ago, which is the ordinary race, since the
	// despawn is reliable and this is not - would silence the other seven.
	void OnCarStates(PeerId peer, const C_CarStates &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_CarStates out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.count         = 0;

		const uint8_t n = in.count < MAX_CAR_STATES ? in.count : MAX_CAR_STATES;
		for (uint8_t i = 0; i < n; ++i) {
			// NoteCarState is what enforces ownership, the same way
			// Session::RemoveCar does for a despawn: a snapshot about
			// somebody else's car says something about the sender, not the
			// car. It also keeps the session's own copy current, which is
			// what a latecomer's backfill is built from.
			if (!m_session.NoteCarState(in.cars[i], p->id))
				continue;
			if (AmbientCar *car = m_session.FindCar(in.cars[i].netId))
				car->history.Note(in.hdr.sendTimeMs, in.cars[i].pos, p->id);
			// The horn bit follows its row to wherever the row lands. Not
			// kept in the session: a honk is over long before a latecomer
			// could be told about it.
			if (CarStateHornSet(in.hornMask, i))
				out.hornMask |= CarStateHornBit(out.count);
			// The siren too, for the same reason. It is a state rather than a
			// honk, but the host restates it on every row, so a latecomer
			// has it from the first batch he gets.
			if (CarStateSirenSet(in.sirenMask, i))
				out.sirenMask |= CarStateSirenBit(out.count);
			out.cars[out.count++] = in.cars[i];
		}

		if (out.count == 0)
			return;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// A hello waiting on its password (protocol.h, C_Password).
	struct PendingHello {
		PeerId   peer = 0;
		C_Hello  hello{};
		uint32_t atMs = 0;
	};

	void OnHello(PeerId peer, const C_Hello &hello) {
		if (m_session.FindByPeer(peer))
			return;   // duplicate hello, ignore
		if (m_lobby.Has(peer))
			return;   // a launcher is no player; its game connects on its own

		// The version first: a client that speaks another protocol is told
		// that, not asked for a password it may never have heard of.
		if (!m_password.empty() && hello.protocolVersion == PROTOCOL_VERSION) {
			for (const PendingHello &h : m_pendingHellos)
				if (h.peer == peer)
					return;   // duplicate hello, still waiting
			PendingHello held;
			held.peer  = peer;
			held.hello = hello;
			held.atMs  = NowMs();
			m_pendingHellos.push_back(held);
			return;
		}
		AcceptHello(peer, hello);
	}

	void OnPassword(PeerId peer, const C_Password &in) {
		for (size_t i = 0; i < m_pendingHellos.size(); ++i) {
			if (m_pendingHellos[i].peer != peer)
				continue;
			const PendingHello held = m_pendingHellos[i];
			m_pendingHellos.erase(m_pendingHellos.begin() + static_cast<std::ptrdiff_t>(i));
			const std::string given(in.password, strnlen(in.password, PASSWORD_LEN));
			if (given == m_password)
				AcceptHello(peer, held.hello);
			else
				RejectHello(peer, held.hello, REJECT_BAD_PASSWORD, "wrong password");
			return;
		}
		// Nothing waiting: a server with no password, or one already let in.
	}

	void RejectHello(PeerId peer, const C_Hello &hello, RejectReason reject, const char *why) {
		S_Welcome welcome;
		InitHeader(welcome, NowMs());
		welcome.reject = reject;
		m_net.SendTo(peer, welcome, CH_EVENT);
		m_net.Disconnect(peer, LEAVE_KICKED);
		const std::string nick = SanitizeText(hello.nick, NICK_LEN);
		if (reject == REJECT_BAD_VERSION)
			Log(LogKind::Warn, "turned %s away: their CoopIII speaks protocol %u and "
			                   "this server %u; the two have to match, which in "
			                   "practice means the same CoopIII release (this server "
			                   "is " COOPIII_VERSION ")",
			    nick.empty() ? "a player" : nick.c_str(),
			    static_cast<unsigned>(hello.protocolVersion),
			    static_cast<unsigned>(PROTOCOL_VERSION));
		else if (reject == REJECT_FULL)
			Log(LogKind::Warn, "turned %s away: %s (%u slots)",
			    nick.empty() ? "a player" : nick.c_str(), RejectText(reject),
			    static_cast<unsigned>(MAX_PLAYERS));
		else
			Log(LogKind::Warn, "turned %s away: %s", nick.empty() ? "a player" : nick.c_str(),
			    why ? why : RejectText(reject));
	}

	void AcceptHello(PeerId peer, const C_Hello &hello) {
		RejectReason reject = REJECT_NONE;
		Player *p = m_session.AddPlayer(peer, hello.nick, hello.modelId,
		                                hello.protocolVersion, reject);
		if (!p) {
			RejectHello(peer, hello, reject, nullptr);
			return;
		}
		m_net.SetMember(peer, true);
		if (peer < SERVER_PEERS)
			m_connectedAtMs[peer] = 0;

		S_Welcome welcome;
		InitHeader(welcome, NowMs());
		welcome.reject = REJECT_NONE;

		welcome.playerId   = p->id;
		welcome.netId      = p->netId;
		welcome.maxPlayers = MAX_PLAYERS;
		welcome.snapshotHz = SNAPSHOT_HZ;
		welcome.hour         = m_session.Clock().Hour();
		welcome.minute       = m_session.Clock().Minute();
		welcome.weather      = m_session.Weather();
		welcome.weatherOld   = m_session.WeatherOld();
		welcome.hostPlayerId = m_session.HostId();
		// The session's own rules. All three are here because each governs
		// something the server cannot see: an explosion is the one kind of
		// damage that never passes through here to be refused, ammunition for
		// a weapon nobody is holding is refused here but applied there, and a
		// wanted level never passes through here at all (protocol.h,
		// SessionFlags).
		welcome.flags        = 0;
		if (m_session.FriendlyFire())
			welcome.flags |= SESSION_FRIENDLY_FIRE;
		if (m_session.AmmoSync())
			welcome.flags |= SESSION_AMMO_SYNC;
		welcome.flags        = FlagsWithWantedRule(welcome.flags, m_session.WantedRule());
		// docs/roadmap.md 5.10. Same shape as the wanted rule above and for
		// the same reason: it governs something inside each client's own
		// CDarkel that never passes through here.
		welcome.flags        =
		    FlagsWithRampageRule(welcome.flags, m_session.RampageRuleValue());
		// And the cheat rule, which the server can only half enforce: a
		// personal cheat never leaves the machine it was typed on, so `off`
		// for one of those is that machine's to carry out. docs/cheats.md.
		welcome.flags        =
		    FlagsWithCheatRule(welcome.flags, m_session.CheatRuleValue());
		m_net.SendTo(peer, welcome, CH_EVENT);
		// The money rule has no bit left in those flags, so it follows the
		// welcome on its own - and with money off, not at all.
		if (m_session.MoneyRuleValue() != MONEY_RULE_OFF)
			m_net.SendTo(peer, m_session.MoneyFor(p->id, INVALID_PLAYER, 0, NowMs()),
			             CH_EVENT);

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
		for (const S_PlayerLook &look : back.looks)
			m_net.SendTo(peer, look, CH_EVENT);
		for (const S_PlayerAway &away : back.aways)
			m_net.SendTo(peer, away, CH_EVENT);
		for (const S_PlayerAmmo &ammo : back.ammo)
			m_net.SendTo(peer, ammo, CH_EVENT);
		for (const S_VehicleSpawn &spawn : back.vehicles)
			m_net.SendTo(peer, spawn, CH_EVENT);
		for (const S_EnterVehicle &seat : back.seats)
			m_net.SendTo(peer, seat, CH_EVENT);
		for (const S_VehicleCustody &custody : back.custodies)
			m_net.SendTo(peer, custody, CH_EVENT);
		for (const S_PedSpawn &ped : back.peds)
			m_net.SendTo(peer, ped, CH_EVENT);
		// After every spawn, not interleaved with them: the joiner records a
		// death against a row it must already have, and CH_EVENT being
		// ordered is what makes "already have" true without a retry.
		for (const S_PedDeath &death : back.pedDeaths)
			m_net.SendTo(peer, death, CH_EVENT);
		for (const S_CarSpawn &car : back.cars)
			m_net.SendTo(peer, car, CH_EVENT);
		// Which pickups the session has already had taken. Without it the
		// joiner is the one player who can still see - and try to collect -
		// every hidden package the group has been through.
		for (const S_PickupTaken &taken : back.pickups)
			m_net.SendTo(peer, taken, CH_EVENT);
		// And which cars nobody owns are already burnt out, for the same
		// reason. This is the whole point of docs/roadmap.md 5.8: without it
		// a joiner is handed a pristine car standing where a shell is on
		// every other screen.
		for (const S_UnownedBlowUp &blast : back.unownedWrecks)
			m_net.SendTo(peer, blast, CH_EVENT);
		// And what shape the session's cars are in. After the spawns above on
		// purpose: the client holds the word in the car's row and applies it
		// once the model has streamed in, so either order works, but arriving
		// second means it is never the thing that creates the row.
		for (const S_VehicleDamage &dmg : back.vehicleDamage)
			m_net.SendTo(peer, dmg, CH_EVENT);
		// And the station each car's radio is on, after the spawns for the
		// same reason.
		for (const S_VehicleRadio &radio : back.radios)
			m_net.SendTo(peer, radio, CH_EVENT);
		// And the bombs they carry, after the spawns for the same reason.
		for (const S_VehicleBomb &bomb : back.bombs)
			m_net.SendTo(peer, bomb, CH_EVENT);
		// And whose a mission's bomb is, after the bomb itself.
		for (const S_MissionBomb &bomb : back.missionBombs)
			m_net.SendTo(peer, bomb, CH_EVENT);
		// And the alarms still going and where each tank's turret points, after
		// the spawns for the same reason.
		for (const S_VehicleAlarm &alarm : back.alarms)
			m_net.SendTo(peer, alarm, CH_EVENT);
		for (const S_VehicleAim &aim : back.aims)
			m_net.SendTo(peer, aim, CH_EVENT);
		// And which doors are currently open for somebody. Without it a
		// joiner is the one player whose safehouse door is shut while
		// somebody is standing inside it, until that somebody walks away -
		// at which point they would be told a door they never saw open had
		// closed.
		for (const S_GarageState &garage : back.garages)
			m_net.SendTo(peer, garage, CH_EVENT);
		// The gates, for the same reason as the doors.
		for (const Player &q : m_session.Players()) {
			if (!q.active || q.id == p->id || q.gateMask == 0)
				continue;
			S_GateState g{};
			InitHeader(g, NowMs());
			g.playerId  = q.id;
			g.body.open = q.gateMask;
			m_net.SendTo(peer, g, CH_EVENT);
		}
		// And the street objects somebody near them still sees broken. The
		// joiner's own copies are built pristine; it applies these to any it
		// already has and asks again for the rest when it builds them.
		for (size_t i = 0; i < OBJECT_RECORD_CAPACITY; ++i)
			if (m_objects.Rows()[i].used)
				SendObjectRecord(peer, m_objects.Rows()[i]);
		// And the cheats every machine is running, at the state the last one
		// left them. Without it a joiner walks into a riot as the one screen
		// where nobody is rioting, and at normal speed while everybody else
		// is in slow motion. docs/cheats.md.
		for (const S_Cheat &cheat : back.cheats)
			m_net.SendTo(peer, cheat, CH_EVENT);

		// ...and tell everyone else about the newcomer. Same packet shape,
		// built the same way, so "what a player looks like on the wire" has
		// one answer. Their position bit is clear: nobody has heard from them
		// yet, and the origin is water.
		S_PlayerJoin live = m_session.MakeJoin(*p, NowMs());
		live.flags        = static_cast<uint8_t>(live.flags | PJF_ARRIVED);
		m_net.Broadcast(live, CH_EVENT, peer);

		// Everybody is in the session's mission, somebody who arrives in the
		// middle of one too (missions.md 11). After the join, so nobody hears
		// of a participant they have not been told about. Every joiner hears
		// the state, idle or not: it is also how a client learns this server
		// shares missions at all, and claims nothing from one that doesn't.
		if (m_session.Mission().Join(p->id))
			BroadcastMissionState();
		else
			m_net.SendTo(peer, MissionStateNow(), CH_EVENT);

		Log(LogKind::Join, "%s joined (slot %u, net %u); backfilled %zu player(s), "
		            "%zu vehicle(s), %zu seat(s)",
		            p->nick.c_str(), p->id, p->netId, back.players.size(),
		            back.vehicles.size(), back.seats.size());
		if (m_session.HostId() == p->id)
			Log(LogKind::Info, "%s is the host; the session's clock is theirs",
			            p->nick.c_str());
		NoteSkyHolder();
		// The lobby sees who is playing.
		BroadcastLobby();
	}

	// Player changed model. We store it, not just relay it, because the join
	// packet includes model id and a later joiner needs to hear what everyone
	// looks like now, not what they looked like at connect time.
	void OnPlayerModel(PeerId peer, const C_PlayerModel &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p || p->modelId == in.modelId)
			return;

		Log(LogKind::Detail, "%s is now model %u (was %u)", p->nick.c_str(),
		            in.modelId, p->modelId);
		p->modelId = in.modelId;

		S_PlayerModel out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.modelId  = in.modelId;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// What their model 0 is loaded as. Stored for joiners, same as the model.
	void OnPlayerLook(PeerId peer, const C_PlayerLook &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NotePlayerLook(*p, in.look))
			return;

		Log(LogKind::Detail, "%s is wearing '%s'", p->nick.c_str(), p->look);
		m_net.Broadcast(m_session.MakeLook(*p, in.hdr.sendTimeMs), CH_EVENT, peer);
	}

	// Their menu went up or down. Stored for joiners, same as the look.
	void OnPlayerAway(PeerId peer, const C_PlayerAway &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NotePlayerAway(*p, in.away != 0))
			return;
		m_net.Broadcast(m_session.MakeAway(*p, in.hdr.sendTimeMs), CH_EVENT, peer);
	}

	// A weapon slot this player is not holding changed. Stored as well as
	// relayed, same as the model above and for the same reason: a joiner has
	// to be told what everyone is carrying, and an event only ever reaches
	// whoever was connected when it went out.
	//
	// This is where the server switch is enforced. With ammo sync off, the
	// packet is dropped on the floor and nobody's copy of this player is
	// ever handed a real number - which is the pre-change behaviour, not a
	// degraded version of it.
	void OnPlayerAmmo(PeerId peer, const C_PlayerAmmo &in) {
		if (!m_session.AmmoSync())
			return;

		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// Unchanged, or a slot number that is not a weapon. Either way there
		// is nothing to pass on.
		if (!m_session.NoteAmmo(*p, in.slot))
			return;

		S_PlayerAmmo out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.slot     = in.slot;
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

	// A mine that went off on the sender's machine (protocol.h, C_MineBlast).
	// Nobody owns a mine, so there is nothing to check but the place: a
	// position that is not a number or not in the world is dropped, the way
	// the client would drop it.
	void OnMineBlast(PeerId peer, const C_MineBlast &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !MineBlastPlaceSane(in.pos))
			return;
		S_MineBlast out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.pos      = in.pos;
		m_net.Broadcast(out, CH_EVENT, peer);
		Log(LogKind::Detail, "%s's mine went off at %.1f %.1f %.1f", p->nick.c_str(), in.pos.x,
		    in.pos.y, in.pos.z);
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

		// Nobody hurts somebody sitting in the car with them: a passenger's
		// round goes out of the car, never into it (session.h, ShareACar).
		if (ShareACar(*attacker, *victim))
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
		// Read before the record is cleared: NotePlayerDied takes them out of
		// whatever they were in, which is what hands the car's settle to
		// their machine, and afterwards there is nothing left to name.
		const uint16_t wasIn = p->vehicleNetId;
		m_session.NotePlayerDied(*p, in.animId);
		// A player who dies at the wheel is the case this matters most in.
		// They cannot get out of a rolling car by hand - CVehicle::CanPedExitCar
		// refuses anything above 0.005 - so dying in one is a common way for a
		// car to lose its driver mid-motion, which is exactly the state the
		// old code froze for the rest of the session.
		AnnounceCustody(wasIn, in.hdr.sendTimeMs);

		const Player *killer = m_session.FindByNetId(in.killerNetId);
		Log(LogKind::Detail, "%s died%s%s", p->nick.c_str(),
		            killer ? ", killed by " : "", killer ? killer->nick.c_str() : "");

		S_Death out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId    = p->id;
		out.killerNetId = in.killerNetId;
		out.animId      = in.animId;
		m_net.Broadcast(out, CH_EVENT, peer);

		// Dead, they can't start anything.
		m_vote.CancelFor(p->id);
		TickRampageVote(NowMs());
		OrderMissionFail(*p, MISSION_FAIL_DIED);
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
		const uint16_t wasIn = p->vehicleNetId;
		m_session.NotePlayerRespawned(*p, in.body.pos, in.body.heading);
		// Every copy of them starts again at the hospital too, and a probe of
		// one taken before its first sample would be measured against the
		// corpse.
		p->history.Clear();
		AnnounceCustody(wasIn, in.hdr.sendTimeMs);

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
		NoteSnapshot(*p, in.hdr.sendTimeMs, in.body);

		S_PlayerState out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// The same snapshot from somebody standing on something that moves
	// (docs/protocol.md 1.7.1). A snapshot in every way that matters here; the
	// ride is only passed on, since only a machine with the vehicle can use it.
	void OnPlayerStateRide(PeerId peer, const C_PlayerStateRide &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		NoteSnapshot(*p, in.hdr.sendTimeMs, in.body);

		S_PlayerStateRide out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		out.ride     = in.ride;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	void NoteSnapshot(Player &p, uint32_t sendTimeMs, const PlayerStateBody &body) {
		// Position for the next joiner, and the condition fields that go with
		// it. A snapshot is the only thing that ever tells the session what
		// health somebody is on, and a joiner creating a ped needs that
		// before their first snapshot arrives, not after it.
		m_session.NotePlayerState(p, body);
		p.history.Note(sendTimeMs, body.pos, p.id, MoveSpeedMps(body.moveSpeed));
		// Busted is the death rule's other half, and nothing but the
		// snapshot says it. MissionSlot orders one failure per mission, so
		// every snapshot of the arrest after the first is a no-op.
		if (body.pedState == PEDSTATE_ON_WIRE_ARRESTED)
			OrderMissionFail(p, MISSION_FAIL_BUSTED);
	}

	// What shape a car is in. docs/cardamage.md.
	//
	// The same entitlement gate as the snapshot above, and for the same
	// reason: a car's owner is its driver, and the server has no GTA III
	// running so the one useful thing it can check is whether the reporter is
	// the player it believes is behind the wheel - or, with nobody behind it,
	// the player settling it (S_VehicleCustody), whose engine is the one
	// denting it until C_VehicleSettled. roadmap.md §5.8's other three kinds
	// of car - a parked one, a traffic car, one somebody walked away from and
	// that has finished settling - are named work and are not accepted here
	// yet.
	//
	// Relayed only when it added something, which is what keeps an absolute,
	// near-static state from becoming a stream.
	// A car's bomb, from the machine that simulates it (protocol.h,
	// C_VehicleBomb): the same entitlement as its dents, relayed as it is.
	//
	// Kept on the car as well, so a joiner's copy has it (Session::NoteVehicleBomb).
	void OnVehicleBomb(PeerId peer, const C_VehicleBomb &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NoteVehicleBomb(p->id, in, NowMs()))
			return;
		S_VehicleBomb out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.bombType = in.bombType;
		out.netId    = in.netId;
		out.blame    = in.blame;
		out.fuseMs   = in.fuseMs;
		m_net.Broadcast(out, CH_EVENT, peer);
		Log(LogKind::Detail, "%s's car %u has bomb %u now (player %u's, fuse %u ms)",
		    p->nick.c_str(), in.netId, static_cast<unsigned>(in.bombType),
		    static_cast<unsigned>(in.blame), static_cast<unsigned>(in.fuseMs));
	}

	// A bomb the mission's script fitted to a car (protocol.h,
	// C_MissionBomb), from the mission's owner: the car's bomb is theirs,
	// whoever simulates it. To everybody else, and kept for a joiner.
	void OnMissionBomb(PeerId peer, const C_MissionBomb &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NoteMissionBomb(p->id, in))
			return;
		S_MissionBomb out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.bombType = in.bombType;
		out.netId    = in.netId;
		m_net.Broadcast(out, CH_EVENT, peer);
		Log(LogKind::Detail, "%s's mission fitted bomb %u to car %u; it is theirs on every copy",
		    p->nick.c_str(), static_cast<unsigned>(in.bombType), in.netId);
	}

	// A car's radio, from somebody sitting in it. docs/protocol.md 1.33.
	void OnVehicleRadio(PeerId peer, const C_VehicleRadio &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		uint8_t held = RADIO_STATION_UNKNOWN;
		if (!m_session.NoteVehicleRadio(p->id, in.netId, in.station, held)) {
			// Refused, or nothing new. The sender's copy is already on the
			// station it named, so when the session holds a different one it
			// is told that one, alone, and its copy goes back.
			if (held != RADIO_STATION_UNKNOWN && held != in.station) {
				S_VehicleRadio back{};
				InitHeader(back, in.hdr.sendTimeMs);
				back.playerId = INVALID_PLAYER;
				back.station  = held;
				back.netId    = in.netId;
				m_net.SendTo(peer, back, CH_EVENT);
			}
			return;
		}
		S_VehicleRadio out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.station  = in.station;
		out.netId    = in.netId;
		// To everybody, the sender too: two players turning the dial at once
		// end on the one this took last, on every machine.
		m_net.Broadcast(out, CH_EVENT);
		Log(LogKind::Detail, "%s put car %u's radio on station %u", p->nick.c_str(), in.netId,
		    static_cast<unsigned>(in.station));
	}

	// A car's alarm, from the machine simulating it. docs/protocol.md 1.39.
	void OnVehicleAlarm(PeerId peer, const C_VehicleAlarm &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		uint16_t remaining = in.remainingMs;
		if (!m_session.NoteVehicleAlarm(p->id, in.netId, remaining, NowMs()))
			return;
		S_VehicleAlarm out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId    = p->id;
		out.netId       = in.netId;
		out.remainingMs = remaining;
		m_net.Broadcast(out, CH_EVENT, peer);
		if (remaining)
			Log(LogKind::Detail, "%s's car %u has its alarm going, %u ms of it",
			    p->nick.c_str(), in.netId, static_cast<unsigned>(remaining));
		else
			Log(LogKind::Detail, "%s's car %u has stopped its alarm", p->nick.c_str(),
			    in.netId);
	}

	// Where a car's gun points, from its driver. docs/protocol.md 1.39.
	void OnVehicleAim(PeerId peer, const C_VehicleAim &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.NoteVehicleAim(p->id, in.netId, in.gunLR, in.gunUD))
			return;
		S_VehicleAim out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.netId    = in.netId;
		out.gunLR    = in.gunLR;
		out.gunUD    = in.gunUD;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	void OnVehicleDamage(PeerId peer, const C_VehicleDamage &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		VehicleDamageBody merged{};
		if (!m_session.MayReportVehicle(p->id, in.body.netId)) {
			// Or one of the sender's own traffic cars. Anything else is
			// refused here as it always was; OnVehicleState already says that
			// out loud, once.
			if (!m_session.NoteCarDamage(p->id, in.body, merged))
				return;
			if (!m_saidCarDamage) {
				m_saidCarDamage = true;
				Log(LogKind::Detail, "traffic dents travel now: %s's car %u is panels "
				    "%08X doors %04X", p->nick.c_str(), in.body.netId,
				    static_cast<unsigned>(merged.panels),
				    static_cast<unsigned>(merged.doors));
			}
			S_VehicleDamage out{};
			InitHeader(out, in.hdr.sendTimeMs);
			out.playerId = p->id;
			out.body     = merged;
			m_net.Broadcast(out, CH_EVENT, peer);
			return;
		}

		if (!m_session.NoteVehicleDamage(in.body, merged))
			return;

		if (IsDamageReset(merged.panels))
			Log(LogKind::Detail, "%s's vehicle %u came out of a spray shop; its "
			    "damage record is cleared", p->nick.c_str(), in.body.netId);
		else
			Log(LogKind::Detail, "%s's vehicle %u is now panels %08X doors %04X",
			    p->nick.c_str(), in.body.netId, static_cast<unsigned>(merged.panels),
			    static_cast<unsigned>(merged.doors));

		S_VehicleDamage out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = merged;
		// Everyone but the reporter: their own engine did it, which is how
		// they came to be the one telling us.
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A hit one player's machine landed on a car another player is driving,
	// or settling after getting out of it.
	//
	// The mirror image of the three vehicle handlers around it, and the only
	// packet about a claimed car that travels *towards* its owner. Every
	// decision in it is Session::CustodyForHit's or VehicleHitRecipient's - a
	// car nobody has, a sender claiming a hit on the car they drive, a car
	// nobody holds (which becomes the sender's), one already destroyed, an
	// owner who has gone - and the relay does not second-guess any of them,
	// the same division OnPedDamage has with PedDamageRecipient.
	//
	// Point to point, like OnDamage and OnPedDamage: only the owner has
	// anything to do with it. What the rest of the session needs to see - the
	// smoke, the dents, the wreck - reaches them from the driver afterwards, on
	// the snapshot and on C_VehicleDamage and C_VehicleBlowUp.
	void OnVehicleHit(PeerId peer, const C_VehicleHit &in) {
		const Player *attacker = m_session.FindByPeer(peer);
		if (!attacker)
			return;

		// A session car nobody is driving or settling becomes the shooter's
		// to settle, and the hit goes back to them - after the custody, on the
		// same ordered channel, so it lands in a car they already know is
		// theirs. Session::CustodyForHit says why.
		// A shove asks for the car and nothing else: there is no damage in it
		// to deliver, and a car somebody else holds is theirs to move.
		if (in.body.weapon == VEHICLE_HIT_PUSH) {
			if (m_session.MayPush(attacker->id, in.body.netId) &&
			    m_session.CustodyForHit(in.body.netId, attacker->id) ==
			        Session::HitCustody::Granted) {
				AnnounceCustody(in.body.netId, in.hdr.sendTimeMs);
				Log(LogKind::Detail,
				    "vehicle %u had nobody holding it; %s pushed it and is "
				    "settling it now", in.body.netId, attacker->nick.c_str());
			}
			return;
		}

		const Player *owner = nullptr;
		switch (m_session.CustodyForHit(in.body.netId, attacker->id)) {
		case Session::HitCustody::Granted:
			AnnounceCustody(in.body.netId, in.hdr.sendTimeMs);
			Log(LogKind::Detail,
			    "vehicle %u had nobody holding it; %s shot it and is settling it "
			    "now", in.body.netId, attacker->nick.c_str());
			owner = attacker;
			break;
		case Session::HitCustody::AlreadyTheirs:
			owner = attacker;
			break;
		case Session::HitCustody::NotTheirs:
			owner = m_session.VehicleHitRecipient(in.body.netId, attacker->id);
			break;
		}
		if (!owner)
			return;

		// The weapon is NOT bounded here, and that is the same position
		// OnPedDamage and OnDamage take about the identical field. It is
		// deliberate rather than an oversight: what counts as a cause a shooter
		// may decide is a fact about the engine, it lives in
		// client/src/game/combat.h beside the disassembly that proves it, and
		// the receiving client checks it before it calls CVehicle::
		// InflictDamage. A second copy of that list in here would be a second
		// thing to keep in step with a binary this process has never loaded.
		//
		// The amount is not bounded here either, for the same reason plus one:
		// the ceiling that matters is a memory-safety one (a NaN reaching
		// m_fHealth and from there the car's matrix), and it belongs on the
		// machine that is about to do the writing.

		S_VehicleHit out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.attackerId = attacker->id;
		out.body       = in.body;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// A hit on a replica of somebody's traffic, sent on to the machine hosting
	// the car. Same shape as OnVehicleHit and the same position on bounds: the
	// weapon and the amount are checked by the client that is about to call
	// the engine. Who gets it is Session::CarHitRecipient's decision.
	void OnCarHit(PeerId peer, const C_CarHit &in) {
		const Player *attacker = m_session.FindByPeer(peer);
		if (!attacker)
			return;

		const Player *owner = m_session.CarHitRecipient(in.body.netId, attacker->id);
		if (!owner)
			return;

		S_CarHit out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.attackerId = attacker->id;
		out.body       = in.body;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// ---- police helicopters ------------------------------------------------
	//
	// protocol.h, entry 32. The sender of a state or a gone is the
	// owner by definition - it is his own engine's helicopter - so the only
	// checks are the ones Session makes: a real police slot, and not a
	// serial he has already said is finished.
	void OnHeliState(PeerId peer, const C_HeliState &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NoteHeliState(p->id, in.body))
			return;
		S_HeliState out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.body          = in.body;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	void OnHeliGone(PeerId peer, const C_HeliGone &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.NoteHeliGone(p->id, in.body))
			return;
		S_HeliGone out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.body          = in.body;
		// A credit that names the owner, or somebody who has gone, names
		// nobody. The shooter's machine is the only one that acts on it.
		if (out.body.creditPlayerId != INVALID_PLAYER &&
		    !m_session.MayCreditHeli(p->id, out.body.creditPlayerId))
			out.body.creditPlayerId = INVALID_PLAYER;
		if (out.body.reason == HELI_GONE_SHOT_DOWN)
			Log(LogKind::Info, "%s's police helicopter was shot down%s", p->nick.c_str(),
			    out.body.creditPlayerId != INVALID_PLAYER ? " by another player" : "");
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnHeliHit(PeerId peer, const C_HeliHit &in) {
		const Player *attacker = m_session.FindByPeer(peer);
		if (!attacker)
			return;
		const Player *owner = m_session.HeliHitRecipient(in.body, attacker->id);
		if (!owner)
			return;
		S_HeliHit out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.attackerId = attacker->id;
		out.body       = in.body;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// A round the owner's helicopter fired, for everybody else to see and
	// hear. The damage already happened on the owner's machine.
	void OnHeliShot(PeerId peer, const C_HeliShot &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.MayRelayHeliShot(p->id, in.body))
			return;
		S_HeliShot out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerPlayerId = p->id;
		out.body          = in.body;
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
					Log(LogKind::Warn, "dropping %s's snapshots for vehicle %u - "
					            "they are in seat %u of it, not driving it",
					            p->nick.c_str(), in.body.netId, p->seat);
				else
					Log(LogKind::Warn, "dropping %s's snapshots for vehicle %u - "
					            "the session has them in vehicle %u", p->nick.c_str(),
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
		// A wreck takes one update: where it is coming to rest, from the
		// machine settling it, flagged VEH_WRECKED (client/src/game/wreck.h).
		// Anything else still arriving for it was sampled before the blast.
		Vehicle *known = m_session.FindVehicle(in.body.netId);
		if (known && known->destroyed) {
			if (!m_session.NoteWreckState(p->id, in.body))
				return;
			known->history.Note(in.hdr.sendTimeMs, in.body.pos, p->id,
			                    MoveSpeedMps(in.body.moveSpeed));
			S_VehicleState wreck;
			InitHeader(wreck, in.hdr.sendTimeMs);
			wreck.playerId = p->id;
			wreck.body     = in.body;
			m_net.Broadcast(wreck, CH_SNAPSHOT, peer);
			return;
		}
		// And a wreck's settle that overtook the event that makes it one: the
		// C_VehicleBlowUp or C_UnownedBlowUp is reliable and right behind it.
		// Taken here it would destroy the car without the event, and the event
		// would then find a wreck and be dropped unrelayed.
		if (in.body.flags & VEH_WRECKED)
			return;
		m_session.NoteVehicleState(in.body);
		if (known && known->destroyed)
			Log(LogKind::Detail, "vehicle %u is wrecked; joiners will not be told "
			            "about it", known->netId);
		if (Vehicle *v = m_session.FindVehicle(in.body.netId))
			v->history.Note(in.hdr.sendTimeMs, in.body.pos, p->id,
			                MoveSpeedMps(in.body.moveSpeed));

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
	// Say no to a claim, rather than saying nothing.
	//
	// A claim is a request, and a request that is sometimes answered and
	// sometimes silently dropped is exactly the shape of the bug the
	// 2026-09-22 logs caught: the client sends it once
	// (Client::m_vehicleClaimPending stops a resend) and then waits forever,
	// sitting at the wheel of a car the session says belongs to nobody. Both
	// ways out of OnEnterVehicle's claim arm now carry an answer.
	//
	// The answer is an ordinary S_EnterVehicle addressed to the claimer alone
	// with netId INVALID_NETID - a value the server could never have meant
	// before, so it costs no new opcode and no new struct. Only the claimer
	// gets it: nobody else was ever told the car existed.
	void RefuseVehicleClaim(PeerId peer, const Player &p, uint32_t sendTimeMs,
	                        const char *why) {
		S_EnterVehicle out;
		InitHeader(out, sendTimeMs);
		out.playerId   = p.id;
		out.body       = EnterVehicleBody{};
		out.body.netId = INVALID_NETID;
		m_net.SendTo(peer, out, CH_EVENT);

		Log(LogKind::Detail, "refused %s's vehicle claim: %s", p.nick.c_str(), why);
	}

	void OnEnterVehicle(PeerId peer, const C_EnterVehicle &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;   // nobody to answer

		Vehicle *v = m_session.FindVehicle(in.body.netId);
		if (!v) {
			// A number the session knows as traffic rather than as a session
			// car. Somebody has taken the wheel of a car another machine's
			// engine made, and that is an ownership change the ambient roster
			// cannot describe - it has an owner and no seats, so the driver
			// would be invisible to it and every observer would go on drawing
			// their ped in the road. protocol.h, S_CarPromoted.
			//
			// Answered before the "never heard of" refusal below on purpose:
			// netIds are one space, so a number that names an AmbientCar is
			// not a stale or invented one, it is a car the session has and
			// files under the other kind.
			if (in.body.netId != INVALID_NETID && in.body.seat == 0) {
				uint8_t        wasOwner = INVALID_PLAYER;
				AmbientCarBody body{};
				v = m_session.PromoteCar(in.body.netId, p->id, wasOwner, body);
				if (v) {
					// Everyone, the claimer included, and BEFORE the
					// S_EnterVehicle below. Same ordering rule as the jack:
					// the two ride one reliable ordered channel, so this is
					// what stops a machine being told about a seat in a car
					// it still has filed as traffic.
					S_CarPromoted promoted;
					InitHeader(promoted, in.hdr.sendTimeMs);
					promoted.netId            = v->netId;
					promoted.driverPlayerId   = p->id;
					promoted.wasOwnerPlayerId = wasOwner;
					promoted.body             = body;
					m_net.Broadcast(promoted, CH_EVENT);
					Log(LogKind::Detail,
					    "traffic car %u is a session car now - %s took the "
					    "wheel of it and it was player %u's",
					    v->netId, p->nick.c_str(),
					    static_cast<unsigned>(wasOwner));
				}
			}
		}

		if (!v) {
			if (in.body.netId != INVALID_NETID) {
				RefuseVehicleClaim(peer, *p, in.hdr.sendTimeMs,
				                   "it names a car the session has never heard "
				                   "of - stale, or made up");
				return;
			}

			v = m_session.AddVehicle(in.body.modelId, in.body.colour1,
			                         in.body.colour2, in.body.pos, in.body.rot);
			if (!v) {
				RefuseVehicleClaim(peer, *p, in.hdr.sendTimeMs,
				                   "the session is already tracking as many "
				                   "cars as it will");
				return;
			}

			// Set here rather than passed to AddVehicle, so Session's
			// signature stays where the join/backfill work left it. The
			// claimer's machine is the only one that knows which extras this
			// car has - its own engine chose them. docs/protocol.md §1.12.
			v->extra1 = in.body.extra1;
			v->extra2 = in.body.extra2;
			// And the car generator it was parked on, which every other machine
			// has a car of its own on (docs/protocol.md 1.44).
			v->parkedSlot = in.body.parkedSlot;

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
			spawn.parkedSlot = v->parkedSlot;
			m_net.Broadcast(spawn, CH_EVENT, peer);

			Log(LogKind::Detail, "vehicle %u claimed by %s (model %u, extras %d/%d)",
			            v->netId, p->nick.c_str(), v->modelId,
			            static_cast<int>(v->extra1), static_cast<int>(v->extra2));
		}

		// Every seat, not just the driver's. A passenger's seat has one
		// carrier on the wire and that is this packet, so a session that does
		// not write it down is a session that cannot tell the next joiner
		// about it - see Player::seat.
		const uint16_t wasIn     = p->vehicleNetId;
		const uint8_t  displaced = m_session.NoteEnterVehicle(*p, *v, in.body.seat);
		p->warnedVehicleAuthority = false;

		// Straight from one car into another with no exit in front of it. The
		// session has taken them out of the old one and, if they drove it,
		// made them its custodian; everybody else is told both, or they go on
		// seating the player in two cars and nobody settles the one left.
		if (wasIn != INVALID_NETID && wasIn != v->netId) {
			S_ExitVehicle left;
			InitHeader(left, in.hdr.sendTimeMs);
			left.playerId = p->id;
			left.netId    = wasIn;
			m_net.Broadcast(left, CH_EVENT, peer);
			AnnounceCustody(wasIn, in.hdr.sendTimeMs);
		}

		// A car that has changed hands. Session::NoteEnterVehicle has already
		// taken the previous driver out of it and says who that was; this is
		// the half only the fan-out can do, which is telling them.
		//
		// Sent before the enter, and to everybody including the loser. Before,
		// because the two packets ride the same reliable ordered channel, so
		// this ordering is the one thing that guarantees no client ever holds
		// two owners for one car - not even for one packet. To the loser,
		// because their own process never saw the jack: the animation, the
		// door and the drag-out all happened in the claimer's game, and the
		// only thing that can reach the victim is a packet. Until this
		// existed, the victim's client kept m_localVehicleNetId pointing at a
		// car it no longer owned, which made it refuse every snapshot the new
		// owner sent - the car sat in the street on that screen while it was
		// driven away on the other.
		//
		// An ordinary S_ExitVehicle, not a new event. "You are no longer in
		// that car" is exactly what it says, it is what the client already
		// handles on both arms, and a jack-specific packet would need the
		// client to do something different with it - which it does not.
		if (displaced != INVALID_PLAYER) {
			S_ExitVehicle lost;
			InitHeader(lost, in.hdr.sendTimeMs);
			lost.playerId = displaced;
			lost.netId    = v->netId;
			m_net.Broadcast(lost, CH_EVENT);
			Log(LogKind::Detail,
			    "vehicle %u changed hands: %s took it from player %u, who has "
			    "been told they are out of it",
			    v->netId, p->nick.c_str(), static_cast<unsigned>(displaced));
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
		// Its driver, or with none the one settling it. A driver the blast
		// killed may already be out of the seat here: an older client sends
		// the death first, NotePlayerDied takes them out of the car and makes
		// their machine its custodian, and the blast then comes from exactly
		// that machine.
		if (!m_session.MayReportVehicle(p->id, v->netId))
			return;

		// The whole of a wreck's bookkeeping, passengers included, and who
		// settles it now said out loud, after the blast: the reporter, whose
		// engine threw the car up and knows where it comes down
		// (Session::DestroyVehicle).
		m_session.DestroyVehicle(v->netId, p->id, NowMs());
		v->pos = in.body.pos;
		v->rot = in.body.rot;

		Log(LogKind::Detail, "vehicle %u blown up by %s", v->netId, p->nick.c_str());

		S_VehicleBlowUp out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
		AnnounceCustody(v->netId, in.hdr.sendTimeMs);
	}

	// "I am getting into that car." Relayed, and written down nowhere.
	//
	// This is the one packet in the vehicle seam that changes no session
	// state at all, and that is the whole of its design rather than a gap in
	// it. An entry can be abandoned - the player is shot halfway in, the car
	// drives off, he changes his mind - and nothing ever retracts this: the
	// only thing that confirms an entry is the C_EnterVehicle at the end of
	// it, which is where the seat, the driver and the car's ownership are
	// still decided. A server that recorded a seat from here would hand a car
	// to a player who never got in, and §2.8.3 would be reading a record that
	// no packet ever corrects.
	//
	// So the two tests are only about whether the packet is worth forwarding:
	// the sender exists, and the car is one the session has. There is
	// deliberately no "is he near it", no "is the seat free" and no
	// arbitration - it decides nothing, so there is nothing to arbitrate.
	void OnEnteringVehicle(PeerId peer, const C_EnteringVehicle &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.FindVehicle(in.body.netId))
			return;   // a car we have never heard of; nobody could animate it

		S_EnteringVehicle out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		// Everybody but the sender: his own engine is the one playing it.
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// The same for a jack, and relayed on the same terms: it decides nothing,
	// so there is nothing to arbitrate. The one difference is that a traffic
	// car counts as a car the session has - a jack is how a player takes one,
	// and it only becomes a session car at the claim.
	void OnJackingVehicle(PeerId peer, const C_JackingVehicle &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.FindVehicle(in.body.netId) && !m_session.FindCar(in.body.netId))
			return;

		S_JackingVehicle out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
		Log(LogKind::Detail, "%s is pulling somebody out of %s %u", p->nick.c_str(),
		    m_session.FindVehicle(in.body.netId) ? "vehicle" : "traffic car",
		    in.body.netId);
	}

	// A car nobody owns was destroyed. docs/roadmap.md 5.8.
	//
	// There is no authority test here and its absence is the design, not an
	// omission. OnVehicleBlowUp above asks `driverPlayerId == p->id` because
	// that car has an owner and a report from anybody else is a lie about
	// somebody else's property. This car has no owner: the map put it there,
	// nobody claimed it, and the only machine that can possibly know it blew
	// up is one that had it streamed in - which the host may well not be,
	// since GTA III streams around each player's own position
	// (docs/roadmap.md 2.1). Requiring the host to report it would mean
	// nobody reports a car on the far side of the city from them.
	//
	// So anyone who was there may say so, and the protection is that saying
	// so is worth nothing: the key names a car the map already put on every
	// machine, the packet carries no position and no condition, and the worst
	// a malicious client can do is destroy a parked car - which it could do
	// anyway, with a rocket, and everyone would see that too.
	//
	// First report wins. Two machines both correctly noticing the same
	// explosion is the normal case, not a race, because the explosion itself
	// was already replayed on both.
	void OnUnownedBlowUp(PeerId peer, const C_UnownedBlowUp &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		if (!m_session.NoteUnownedBlowUp(in.key, p->id, NowMs()))
			return;   // already recorded, or a kind this build does not speak

		Log(LogKind::Detail, "unowned car (kind %u, id %u) blown up, reported by %s",
		    static_cast<unsigned>(in.key.kind), static_cast<unsigned>(in.key.id),
		    p->nick.c_str());

		S_UnownedBlowUp out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.reporterPlayerId = p->id;
		out.key              = in.key;
		// Relayed as sent. The server has no opinion about where a car is -
		// it has no GTA III running - so this is the reporter's own reading
		// passed along, exactly like the transform on C_VehicleBlowUp. What
		// the server does decide is whether the reporter was entitled to say
		// anything at all, and NoteUnownedBlowUp above has already done that.
		out.where            = in.where;
		// Everyone but the reporter: their own engine has already done it,
		// which is how they came to be the one telling us.
		m_net.Broadcast(out, CH_EVENT, peer);
		// A session car's wreck is the reporter's to settle now, whoever held
		// it before (Session::DestroyVehicle). Said after the blast, on the
		// same channel, so nobody takes its settle for a live car's snapshots.
		if (in.key.kind == UNOWNED_SESSION)
			AnnounceCustody(in.key.id, in.hdr.sendTimeMs);
	}

	// ---- garages, doors and the Pay'n'Spray --------------------------------
	//
	// docs/protocol.md §1.16. The server arbitrates nothing here and that is
	// not laziness: a garage belongs to the map, not to a player, so there is
	// no owner to check a report against and nothing for a report to be a lie
	// about. Compare OnVehicleState, which does check, because a car *does*
	// have an owner.
	//
	// What the server does is remember, so a joiner can be told.
	void OnGarageState(PeerId peer, const C_GarageState &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// Bits above NUM_GARAGES are dropped rather than the packet. The
		// engine has exactly 32 garages - CGarages::Update's loop is a
		// literal `cmp ebx,20h` - so a bit outside that range is a client
		// this server does not understand, and a door it cannot name is a
		// door nobody can be told about. Masking keeps the doors it *can*
		// name working.
		const uint32_t mask = in.body.deviating & GARAGE_MASK_ALL;
		if (mask == p->garageMask)
			return;   // nothing changed; do not spend a broadcast on it
		p->garageMask = mask;

		S_GarageState out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId       = p->id;
		out.body.deviating = mask;
		// Everyone but the sender. Their own state machine is where this came
		// from, and a client that was handed its own mask back would OR its
		// own opinion into the union it holds its own doors to.
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnRespray(PeerId peer, const C_Respray &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (in.body.garage >= NUM_GARAGES)
			return;

		Log(LogKind::Detail, "%s resprayed at garage %u, colours %u/%u",
		    p->nick.c_str(), static_cast<unsigned>(in.body.garage),
		    static_cast<unsigned>(in.body.colour1),
		    static_cast<unsigned>(in.body.colour2));

		S_Respray out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		// Not recorded for the backfill, and that is deliberate. A respray is
		// a *transition* - it moves a car from dented-and-blue to
		// fixed-and-red - and its result is already the car's ordinary
		// condition from the instant it finishes. A joiner is told the
		// colours a car has now on its spawn packet (S_VehicleSpawn), so
		// replaying the transition would be telling them twice, once with a
		// door animation that finished before they connected.
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// Who simulates a car nobody is driving, told to everybody.
	// protocol.h, S_VehicleCustody.
	//
	// Always sent AFTER the S_ExitVehicle that created the vacancy, on the
	// same reliable ordered channel, which is the whole of the ordering
	// guarantee: no client ever holds a driver and a custodian for one car at
	// the same instant. The mirror of the jack's rule, where the loser's exit
	// goes out before the winner's enter, and for the same reason.
	//
	// Sent even when the custodian is INVALID_PLAYER - "nobody is simulating
	// this" is a real instruction and not an absence. A client that was told
	// it had custody and never told it had lost it would go on streaming a car
	// whose reports the server has already started dropping.
	void AnnounceCustody(uint16_t netId, uint32_t sendTimeMs) {
		if (netId == INVALID_NETID)
			return;
		S_VehicleCustody out;
		InitHeader(out, sendTimeMs);
		out.netId    = netId;
		out.playerId = m_session.CustodianOf(netId);
		out.pad      = 0;
		m_net.Broadcast(out, CH_EVENT);
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

		// And who finishes what the car was doing, if it was doing anything.
		// This is the packet that frees a car left reared up against a wall:
		// without it every machine, including the one that just got out,
		// writes the pose back onto the car after physics, every frame, for
		// ever. Session::NoteExitVehicle has already decided who; this only
		// says it out loud.
		AnnounceCustody(in.netId, in.hdr.sendTimeMs);
	}

	// The custodian reporting that it is finished. protocol.h, C_VehicleSettled.
	//
	// Nothing is taken from the packet but the number. Where the car ended up
	// arrived on the ordinary C_VehicleState stream while the settle was
	// running, which is the same channel and the same record a driver's car
	// uses, and a transform carried here as well would be a second copy of a
	// fact with no way to say which of the two is newer.
	void OnVehicleSettled(PeerId peer, const C_VehicleSettled &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// Refuses anybody but the custodian. A client may end its own
		// ownership and never somebody else's.
		if (!m_session.EndCustody(in.netId, p->id))
			return;
		AnnounceCustody(in.netId, in.hdr.sendTimeMs);
		Log(LogKind::Detail,
		    "vehicle %u has settled - %s is finished with it and every machine "
		    "holds it where it stands now",
		    in.netId, p->nick.c_str());
	}

	// A session car the sender's engine crushed, craned, delivered or stored
	// (protocol.h, C_VehicleRemoved). Everybody else first hears why - which
	// is what has a machine delete a car its own engine made, and merge the
	// lists - and then everybody, the sender included, the ordinary despawn.
	void OnVehicleRemoved(PeerId peer, const C_VehicleRemoved &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		VehicleRemovedBody relay{};
		if (!m_session.RemoveVehicleOnPurpose(p->id, in.body, relay)) {
			Log(LogKind::Detail,
			    "refused %s's removal of vehicle %u (reason %u) - unknown, "
			    "somebody else is driving it, or no such reason",
			    p->nick.c_str(), in.body.netId, static_cast<unsigned>(in.body.reason));
			return;
		}
		S_VehicleRemoved removed;
		InitHeader(removed, in.hdr.sendTimeMs);
		removed.playerId = p->id;
		removed.body     = relay;
		m_net.Broadcast(removed, CH_EVENT, peer);

		S_VehicleDespawn out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.netId = in.body.netId;
		m_net.Broadcast(out, CH_EVENT);
		Log(LogKind::Detail, "vehicle %u taken away by %s's engine (reason %u)",
		    in.body.netId, p->nick.c_str(), static_cast<unsigned>(in.body.reason));
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

		Log(LogKind::Chat, "%s: %s", p->nick.c_str(), text.c_str());
	}

	// The host throwing somebody out from inside the game (protocol.h,
	// C_Kick). Session::MayKick decides; the kick itself is the window's, so
	// everybody reads "was kicked" and the player stays out.
	void OnKick(PeerId peer, const C_Kick &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const Player *target = m_session.FindById(in.playerId);
		switch (m_session.MayKick(p->id, in.playerId)) {
		case Session::KickVerdict::Allowed:
			Log(LogKind::Leave, "%s, the host, is throwing %s out", p->nick.c_str(),
			    target ? target->nick.c_str() : "?");
			Kick(in.playerId);
			return;
		case Session::KickVerdict::NotHost:
			Log(LogKind::Warn, "%s asked to kick slot %u and is not the host - refused",
			    p->nick.c_str(), in.playerId);
			return;
		case Session::KickVerdict::NoSuchPlayer:
			Log(LogKind::Detail, "%s asked to kick slot %u, and nobody is in it",
			    p->nick.c_str(), in.playerId);
			return;
		case Session::KickVerdict::Themselves:
			Log(LogKind::Detail, "%s asked to kick themselves - refused", p->nick.c_str());
			return;
		}
	}

	// ---- the lobby (protocol.h, C_LobbyJoin; lobby.h) -------------------------
	//
	// Launchers waiting before anybody's game is running. Lobby decides who is
	// in it and who its host is; what is here is the door and who hears what.

	void OnLobbyJoin(PeerId peer, const C_LobbyJoin &in) {
		if (m_session.FindByPeer(peer) || m_lobby.Has(peer))
			return;   // a game's connection, or a second knock
		const std::string nick = SanitizeText(in.nick, NICK_LEN);
		RejectReason      reject = REJECT_NONE;
		const Lobby::Member *m   = nullptr;
		if (in.protocolVersion != PROTOCOL_VERSION)
			reject = REJECT_BAD_VERSION;
		else if (!m_password.empty() &&
		         std::string(in.password, strnlen(in.password, PASSWORD_LEN)) != m_password)
			reject = REJECT_BAD_PASSWORD;
		else
			m = m_lobby.Join(peer, in.nick, reject);

		S_LobbyAnswer answer{};
		InitHeader(answer, NowMs());
		answer.reject          = reject;
		answer.lobbyId         = m ? m->id : INVALID_PLAYER;
		answer.protocolVersion = PROTOCOL_VERSION;
		m_net.SendTo(peer, answer, CH_EVENT);
		if (!m) {
			m_net.Disconnect(peer, LEAVE_KICKED);
			Log(LogKind::Warn, "turned %s away from the lobby: %s",
			    nick.empty() ? "a launcher" : nick.c_str(), RejectText(reject));
			return;
		}
		if (peer < SERVER_PEERS)
			m_connectedAtMs[peer] = 0;
		Log(LogKind::Info, "%s is waiting in the lobby%s", m->nick.c_str(),
		    m_lobby.HostId() == m->id ? ", and starts everybody's game" : "");
		BroadcastLobby();
	}

	void OnLobbyStart(PeerId peer, const C_LobbyStart &in) {
		const Lobby::Member *m = m_lobby.FindByPeer(peer);
		if (!m || (in.mode != LOBBY_START_MENU && in.mode != LOBBY_START_NEW_GAME))
			return;
		if (!m_lobby.MayStart(peer, NowMs())) {
			if (m->id != m_lobby.HostId())
				Log(LogKind::Warn, "%s asked to start everybody's game and is not the lobby's "
				    "host - refused", m->nick.c_str());
			return;
		}
		S_LobbyStart out{};
		InitHeader(out, NowMs());
		out.mode      = in.mode;
		out.byLobbyId = m->id;
		for (const Lobby::Member &w : m_lobby.Members())
			if (w.active)
				m_net.SendTo(w.peer, out, CH_EVENT);
		Log(LogKind::Info, "%s started everybody's game%s, for the %zu in the lobby",
		    m->nick.c_str(), in.mode == LOBBY_START_NEW_GAME ? ", a new game" : "",
		    m_lobby.Count());
	}

	// Who is waiting and who is playing, to everybody waiting.
	void BroadcastLobby() {
		if (m_lobby.Count() == 0)
			return;
		S_Lobby out{};
		InitHeader(out, NowMs());
		m_lobby.FillRoster(out);
		for (const Player &p : m_session.Players()) {
			if (!p.active || out.count >= LOBBY_MAX + MAX_PLAYERS)
				continue;
			LobbyEntry &e = out.entries[out.count++];
			e.id          = p.id;
			e.flags       = LOBBY_ENTRY_PLAYING;
			std::strncpy(e.nick, p.nick.c_str(), NICK_LEN - 1);
		}
		for (const Lobby::Member &w : m_lobby.Members())
			if (w.active)
				m_net.SendTo(w.peer, out, CH_EVENT);
	}

	// ---- the session's one mission (protocol.h; missionslot.h) ---------------
	//
	// MissionSlot decides everything; what is here is who hears it.

	void OnMissionClaim(PeerId peer, const C_MissionClaim &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		MissionPresence present[MAX_PLAYERS];
		const size_t    n = m_session.MissionPresences(present);
		const MissionSlot::ClaimAnswer a =
		    m_session.Mission().Claim(p->id, in, present, n, NowMs());
		if (a.leftOut != 0) {
			std::string who;
			for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
				if (a.leftOut & PlayerBit(id))
					if (const Player *m = m_session.FindById(id))
						who += (who.empty() ? "" : ", ") + m->nick;
			Log(LogKind::Info,
			    "%s's start of %s goes on without %s, whose game is still in a mission of its "
			    "own; they come into it once that is over",
			    p->nick.c_str(), MissionName(in.missionHint), who.empty() ? "?" : who.c_str());
		}

		S_MissionClaim out;
		InitHeader(out, NowMs());
		out.launchKey   = in.launchKey;
		out.verdict     = a.verdict;
		out.ownerId     = a.ownerId;
		out.missingMask = a.missing;
		out.pad         = 0;
		m_net.SendTo(peer, out, CH_EVENT);
		BroadcastMissionWaiting();
	}

	void OnMissionStarted(PeerId peer, const C_MissionStarted &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		MissionSlot &slot = m_session.Mission();
		if (!slot.Start(p->id, in.missionNumber, m_session.ReadyMask())) {
			const Player *owner = m_session.FindById(slot.Owner());
			Log(LogKind::Warn, "%s started %s while %s's %s runs - it stays theirs alone",
			    p->nick.c_str(), MissionName(in.missionNumber),
			    owner ? owner->nick.c_str() : "?", MissionName(slot.Number()));
			return;
		}
		Log(LogKind::Info, "%s started %s", p->nick.c_str(), MissionName(in.missionNumber));
		m_session.Campaign().MissionStarted();
		BroadcastMissionState();
		BroadcastMissionWaiting();
	}

	void OnMissionEnded(PeerId peer, const C_MissionEnded &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().End(p->id, in.missionNumber, in.outcome))
			return;
		Log(LogKind::Info, "%s %s %s", p->nick.c_str(),
		    in.outcome == MISSION_OUTCOME_PASSED ? "passed" : "failed",
		    MissionName(in.missionNumber));
		BroadcastMissionState();
		BroadcastMissionWaiting();
		ReleaseMissionCars(NowMs());
	}

	void OnMissionCheckpoint(PeerId peer, const C_MissionCheckpoint &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (p && m_session.Mission().Checkpoint(p->id, in.missingMask, in.where, NowMs()))
			BroadcastMissionWaiting();
	}

	// What the owner's mission shows, for everybody else's engine to show.
	// Relayed as it came, in order on CH_EVENT: a blip's removal must never
	// overtake its creation.
	void OnMissionEffect(PeerId peer, const C_MissionEffect &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().MayRelayEffect(p->id))
			return;
		if (in.body.length > MISSION_EFFECT_CODE)
			return;
		S_MissionEffect out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerId = p->id;
		out.body    = in.body;
		// What the mission has up, for somebody who has just come into it:
		// to them alone.
		if (in.body.onlyTo != 0) {
			const uint8_t id = static_cast<uint8_t>(in.body.onlyTo - 1);
			const Player *to = id < MAX_PLAYERS ? m_session.FindById(id) : nullptr;
			if (to && to->id != p->id)
				m_net.SendTo(to->peer, out, CH_EVENT);
			return;
		}
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// The value behind a widget the owner's HUD shows, for everybody else's to
	// read. Only the running mission's owner has any.
	void OnMissionWidget(PeerId peer, const C_MissionWidget &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().Running() || m_session.Mission().Owner() != p->id)
			return;
		S_MissionWidget out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerId       = p->id;
		out.missionNumber = in.missionNumber;
		out.offset        = in.offset;
		out.value         = in.value;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A participant has loaded what the owner's mission asked for, for the
	// owner's mission to stop waiting on them: to the owner alone, and only
	// from somebody in the running mission.
	void OnMissionReady(PeerId peer, const C_MissionReady &in) {
		const Player *p = m_session.FindByPeer(peer);
		const MissionSlot &slot = m_session.Mission();
		if (!p || !slot.Running() || slot.Owner() == p->id ||
		    (slot.Participants() & PlayerBit(p->id)) == 0 || in.missionNumber != slot.Number())
			return;
		const Player *owner = m_session.FindById(slot.Owner());
		if (!owner)
			return;
		S_MissionReady out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId      = p->id;
		out.missionNumber = in.missionNumber;
		out.readySeq      = in.readySeq;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// A player's game went into a mission of its own, the intro of a new game
	// say, or came out of it (docs/missions.md 11.6). In one, they are nobody
	// the session's mission waits for or shows anything to; out of it, they
	// are in the running one like a joiner, and the owner hands them what it
	// has up. A game that started over while in the running mission is handed
	// it again.
	void OnMissionBusy(PeerId peer, const C_MissionBusy &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		MissionSlot &slot = m_session.Mission();
		const bool   busy = in.busy != 0;
		if (busy != p->missionBusy) {
			p->missionBusy = busy;
			Log(LogKind::Info,
			    busy ? "%s's game is in a mission of its own, and the session's goes on without them"
			         : "%s's game is out of its own mission",
			    p->nick.c_str());
			if (busy ? slot.StandAside(p->id) : slot.Join(p->id)) {
				BroadcastMissionState();
				ReleaseMissionCars(NowMs());
			}
			BroadcastMissionWaiting();
		}
		if (in.fresh != 0 && !busy && slot.Running() && slot.Owner() != p->id &&
		    (slot.Participants() & PlayerBit(p->id)) != 0) {
			if (const Player *owner = m_session.FindById(slot.Owner())) {
				S_MissionHandOver out{};
				InitHeader(out, in.hdr.sendTimeMs);
				out.playerId      = p->id;
				out.missionNumber = slot.Number();
				m_net.SendTo(owner->peer, out, CH_EVENT);
			}
		}
	}

	// A participant who has just come into the running mission, late or back
	// from its own game's mission, asks for what the mission has made: every
	// pedestrian and car of it, to them alone, and then how many. Only while
	// a mission runs; the answer's zeroes say so otherwise.
	void OnMissionCatchUp(PeerId peer, const C_MissionCatchUp &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const MissionSlot &slot = m_session.Mission();
		S_MissionCatchUp   out{};
		InitHeader(out, NowMs());
		out.ownerId       = slot.Running() ? slot.Owner() : INVALID_PLAYER;
		out.missionNumber = slot.Running() ? slot.Number() : MISSION_NONE;
		if (slot.Running() && slot.Owner() != p->id) {
			const MissionCatchUp back = m_session.BuildMissionCatchUp(p->id, NowMs());
			for (const S_PedSpawn &ped : back.peds)
				m_net.SendTo(peer, ped, CH_EVENT);
			for (const S_PedDeath &death : back.pedDeaths)
				m_net.SendTo(peer, death, CH_EVENT);
			for (const S_CarSpawn &car : back.cars)
				m_net.SendTo(peer, car, CH_EVENT);
			out.cars        = static_cast<uint16_t>(back.cars.size());
			out.peds        = static_cast<uint16_t>(back.peds.size());
			out.sessionCars = back.sessionCars;
			Log(LogKind::Detail,
			    "%s caught up with %s: %u car(s) and %u pedestrian(s) of it sent again, %u of its "
			    "cars claimed",
			    p->nick.c_str(), MissionName(slot.Number()), out.cars, out.peds, out.sessionCars);
		}
		(void)in;
		m_net.SendTo(peer, out, CH_EVENT);
	}

	// One of the mission's floating packages was taken on somebody's machine,

	// for the owner's mission to count and everybody else's copy to go. From
	// anybody in the running mission.
	void OnMissionPickup(PeerId peer, const C_MissionPickup &in) {
		const Player *p = m_session.FindByPeer(peer);
		const MissionSlot &slot = m_session.Mission();
		if (!p || !slot.Running() || (slot.Participants() & PlayerBit(p->id)) == 0 ||
		    in.missionNumber != slot.Number())
			return;
		S_MissionPickup out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId      = p->id;
		out.missionNumber = in.missionNumber;
		out.handle        = in.handle;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A kill a participant's machine registered, for the owner's mission to
	// count. From anybody in the running mission but its owner, to the owner
	// alone.
	void OnMissionKill(PeerId peer, const C_MissionKill &in) {
		const Player *p = m_session.FindByPeer(peer);
		const MissionSlot &slot = m_session.Mission();
		if (!p || !slot.Running() || slot.Owner() == p->id ||
		    (slot.Participants() & PlayerBit(p->id)) == 0 || in.missionNumber != slot.Number())
			return;
		const Player *owner = m_session.FindById(slot.Owner());
		if (!owner)
			return;
		S_MissionKill out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId      = p->id;
		out.missionNumber = in.missionNumber;
		out.model         = in.model;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// What a participant's engine answers to the mission's questions, its
	// garages and its planes, for the owner's conditions to hear. From anybody
	// in the running mission but its owner, to the owner alone.
	void OnMissionAnswers(PeerId peer, const C_MissionAnswers &in) {
		const Player *p = m_session.FindByPeer(peer);
		const MissionSlot &slot = m_session.Mission();
		if (!p || !slot.Running() || slot.Owner() == p->id ||
		    (slot.Participants() & PlayerBit(p->id)) == 0 || in.missionNumber != slot.Number())
			return;
		const Player *owner = m_session.FindById(slot.Owner());
		if (!owner)
			return;
		S_MissionAnswers out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId      = p->id;
		out.missionNumber = in.missionNumber;
		out.shotDown      = in.shotDown;
		out.hasCar        = in.hasCar;
		out.resprayed     = in.resprayed;
		m_net.SendTo(owner->peer, out, CH_EVENT);
	}

	// One of the mission's objects broke on somebody's machine, for everybody
	// else's copy to break too. From anybody in the running mission.
	void OnMissionObjectBreak(PeerId peer, const C_MissionObjectBreak &in) {
		const Player *p = m_session.FindByPeer(peer);
		const MissionSlot &slot = m_session.Mission();
		if (!p || !slot.Running() || (slot.Participants() & PlayerBit(p->id)) == 0 ||
		    in.missionNumber != slot.Number())
			return;
		S_MissionObjectBreak out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId      = p->id;
		out.state         = in.state;
		out.missionNumber = in.missionNumber;
		out.global        = in.global;
		out.amount        = in.amount;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// The session's cars whose seats the owner's mission's passengers need,
	// for everybody else to stay out of. Only the running mission's owner
	// has any.
	void OnMissionSeats(PeerId peer, const C_MissionSeats &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().Running() || m_session.Mission().Owner() != p->id ||
		    in.count > MISSION_SEAT_CARS)
			return;
		S_MissionSeats out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerId       = p->id;
		out.count         = in.count;
		out.missionNumber = in.missionNumber;
		for (uint8_t i = 0; i < in.count; ++i)
			out.cars[i] = in.cars[i];
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// The seats in the car the owner's mission put its player in, handed to
	// everybody else. Only the running mission's owner has any to hand out.
	void OnMissionBoard(PeerId peer, const C_MissionBoard &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().Running() || m_session.Mission().Owner() != p->id ||
		    in.netId == INVALID_NETID)
			return;
		S_MissionBoard out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.ownerId       = p->id;
		out.flags         = in.flags;
		out.missionNumber = in.missionNumber;
		out.netId         = in.netId;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			out.seats[id] = in.seats[id];
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// What the owner's mission left behind in the campaign, numbered, kept and
	// sent to everybody, the owner too, who only notes the number. Only from
	// the running mission's owner: it is sent before the end is.
	void OnCampaignDelta(PeerId peer, const C_CampaignDelta &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || !m_session.Mission().Running() || m_session.Mission().Owner() != p->id)
			return;
		if (in.body.valueCount > CAMPAIGN_VALUES || in.body.threadCount > CAMPAIGN_THREADS ||
		    in.body.opLength > MISSION_EFFECT_CODE)
			return;
		CampaignLog &log = m_session.Campaign();
		if (log.Count() >= CAMPAIGN_LOG_MAX || !log.TakePartFor(m_session.Mission().Number())) {
			Log(LogKind::Detail, "%s's %s sent more of the campaign than a mission leaves; "
			    "the rest is not kept", p->nick.c_str(), MissionName(in.body.missionNumber));
			return;
		}
		S_CampaignDelta out;
		InitHeader(out, NowMs());
		out.ownerId  = p->id;
		out.body     = in.body;
		out.body.seq = log.Append(p->id, in.body);
		m_net.Broadcast(out, CH_EVENT);
		Log(LogKind::Detail, "%s's %s left %u global%s and %u thread%s behind (delta %u)",
		    p->nick.c_str(), MissionName(in.body.missionNumber), in.body.valueCount,
		    in.body.valueCount == 1 ? "" : "s", in.body.threadCount,
		    in.body.threadCount == 1 ? "" : "s", out.body.seq);
	}

	// A machine that has just heard the session's mission state: every
	// delta after the last one it has, which is all of them for one that
	// has never been here or whose last log was another run's.
	void OnCampaignSince(PeerId peer, const C_CampaignSince &in) {
		if (!m_session.FindByPeer(peer))
			return;
		for (S_CampaignDelta &d : m_session.Campaign().Since(in.seq)) {
			d.hdr.sendTimeMs = NowMs();
			m_net.SendTo(peer, d, CH_EVENT);
		}
	}

	// A machine's Import/Export and crane lists (C_CarLists). The session's
	// are all of them put together: back to the sender, which asks once on
	// every connection, and to everybody else when they grew.
	void OnCarLists(PeerId peer, const C_CarLists &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const bool grew = MergeCarLists(m_carLists, in.collected);
		S_CarLists out{};
		InitHeader(out, NowMs());
		for (uint8_t i = 0; i < CAR_LISTS; ++i)
			out.collected[i] = m_carLists[i];
		m_net.SendTo(peer, out, CH_EVENT);
		if (!grew)
			return;
		m_net.Broadcast(out, CH_EVENT, peer);
		Log(LogKind::Detail, "%s's game has delivered a car the session's lists lacked", p->nick.c_str());
	}

	// A participant died or was busted. The owner's own death is their
	// engine's to fail, and MissionSlot never orders it.
	void OrderMissionFail(const Player &p, MissionFailReason reason) {
		MissionSlot  &slot = m_session.Mission();
		S_MissionFail fail{};
		if (!slot.FailFor(p.id, reason, &fail))
			return;
		const Player *owner = m_session.FindById(slot.Owner());
		if (!owner)
			return;
		Stamp(fail, NowMs());
		m_net.SendTo(owner->peer, fail, CH_EVENT);
		Log(LogKind::Info, "%s %s, so %s fails for everybody", p.nick.c_str(),
		    reason == MISSION_FAIL_BUSTED ? "was busted" : "died", MissionName(slot.Number()));
	}

	// InitHeader zeroes the packet, so what MissionSlot filled in only gets
	// its header stamped.
	template <class T>
	static void Stamp(T &pkt, uint32_t sendTimeMs) {
		pkt.hdr.opcode     = T::OPCODE;
		pkt.hdr.sendTimeMs = sendTimeMs;
	}

	S_MissionState MissionStateNow() const {
		S_MissionState s = m_session.Mission().State();
		Stamp(s, NowMs());
		s.campaignLog = m_session.Campaign().Id();
		return s;
	}

	void BroadcastMissionState() {
		m_net.Broadcast(MissionStateNow(), CH_EVENT);
		NoteSkyHolder();
	}

	// Who the clock and the sky belong to moves with the session's mission
	// (coopiii/sky.h). Nothing goes out for it: every client works it out
	// from the S_MissionState it has just been sent. This only says so.
	void NoteSkyHolder() {
		const uint8_t holder = m_session.SkyHolderId();
		if (holder == m_skyHolder)
			return;
		const bool first = m_skyHolder == INVALID_PLAYER;
		m_skyHolder      = holder;
		const Player *p  = m_session.FindById(holder);
		if (first || !p)
			return;   // the join says who the host is
		if (holder != m_session.HostId())
			Log(LogKind::Info, "%s's %s runs, so the clock and the sky are theirs until it ends",
			    p->nick.c_str(), MissionName(m_session.Mission().Number()));
		else
			Log(LogKind::Info, "the clock and the sky are the host's, %s's", p->nick.c_str());
	}

	void BroadcastMissionWaiting() {
		S_MissionWaiting w{};
		if (!m_session.Mission().TakeWaitingChange(&w))
			return;
		Stamp(w, NowMs());
		m_net.Broadcast(w, CH_EVENT);
		if (w.what == MISSION_WAIT_NONE)
			return;
		std::string who;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (w.missingMask & PlayerBit(id))
				if (const Player *m = m_session.FindById(id))
					who += (who.empty() ? "" : ", ") + m->nick;
		const Player *owner = m_session.FindById(w.ownerId);
		char busy[96] = "";
		if (w.goesOnInS != 0 && w.what == MISSION_WAIT_START)
			std::snprintf(busy, sizeof busy,
			              ", still in a cutscene of their own; going on without them in %u s",
			              static_cast<unsigned>(w.goesOnInS));
		else if (w.goesOnInS != 0)
			std::snprintf(busy, sizeof busy, "; going on without them in %u s",
			              static_cast<unsigned>(w.goesOnInS));
		Log(LogKind::Detail, "%s is waiting for %s at %s of %s%s",
		    owner ? owner->nick.c_str() : "?", who.empty() ? "?" : who.c_str(),
		    w.what == MISSION_WAIT_START ? "the start" : "a checkpoint",
		    MissionName(w.missionHint), busy);
	}

	// The host's game telling the session what time it is.
	//
	// Relayed rather than merely stored, and the 1 Hz timer is reset on the
	// way out so the free-running broadcast doesn't add a second of age to
	// something that just arrived. With a host connected this path is the
	// only one that ever fires; the timer below is what covers a session
	// whose host is loading, or hasn't reported yet.
	// ---- pickups -----------------------------------------------------------
	//
	// First claim wins, and the reply goes to everybody. docs/pickups.md 4:
	// the client detects, the server arbitrates, the engine awards, in that
	// order - the arbitration has to be settled *before* the winner's engine
	// is allowed to give anything, because a pickup reward cannot honestly be
	// taken back afterwards.

	void OnPickupClaim(PeerId peer, const C_PickupClaim &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// A skull, in a session that shares its rampage, is put to a vote
		// rather than handed over. rampagevote.h.
		if (ClaimGoesToAVote(peer, *p, in.ident))
			return;

		uint8_t movedFrom = INVALID_PLAYER;
		if (m_session.ClaimPickup(p->id, in.ident, NowMs(), &movedFrom) ==
		    Session::PickupVerdict::DENIED) {
			S_PickupDenied out;
			InitHeader(out, NowMs());
			out.ident = in.ident;
			m_net.SendTo(peer, out, CH_EVENT);
			return;
		}
		// A reservation that stood too long went to this claimant: whoever
		// held it hears so, or their machine keeps offering it to its engine.
		if (movedFrom != INVALID_PLAYER)
			if (const Player *old = m_session.FindById(movedFrom))
				DenyPickup(old->peer, in.ident);

		// To the claimant alone. Nobody has picked anything up yet - this is
		// a reservation, and a player who walks past a pickup they turn out
		// not to want must not have deleted it from anyone else's world.
		S_PickupGrant out;
		InitHeader(out, NowMs());
		out.ident = in.ident;
		m_net.SendTo(peer, out, CH_EVENT);
	}

	void OnPickupCollected(PeerId peer, const C_PickupCollected &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// Only the holder's own report turns a reservation into a removal.
		// A duplicate, or a collection from somebody who was never granted
		// it, is dropped here and nothing goes out.
		if (!m_session.NotePickupCollected(p->id, in.ident, NowMs()))
			return;

		// The mission's stash is everybody's own too, but the mission has to
		// hear that somebody took one (protocol.h, PICKUP_F_STASH): everybody
		// is told, and nobody's copy goes.
		if ((in.ident.flags & PICKUP_F_STASH) != 0) {
			S_PickupTaken out;
			InitHeader(out, NowMs());
			out.playerId = p->id;
			out.ident    = in.ident;
			m_net.Broadcast(out, CH_EVENT, peer);
			return;
		}

		// A package under `perplayer` is theirs alone: everybody else's is
		// still where it was.
		if (m_session.PickupIsPerPlayer(in.ident)) {
			if (!m_saidOwnPackage) {
				m_saidOwnPackage = true;
				Log(LogKind::Detail, "%s collected a hidden package; packages are per "
				    "player, so nobody else's is touched", p->nick.c_str());
			}
			return;
		}

		// Everyone except them: their own engine has already removed their
		// copy, which is what produced this message in the first place.
		S_PickupTaken out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.ident    = in.ident;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// ---- rampages - docs/roadmap.md 5.10 -----------------------------------
	//
	// Three handlers, and between them the server's whole part in a rampage:
	// it names the frenzy, it says what the session is playing for, it relays
	// kills, and it picks one ending. It never decides that a rampage was
	// passed - the clients' own CDarkel does that, off a counter they are now
	// all driving with the same events.

	void OnRampageStart(PeerId peer, const C_RampageStart &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		RampageOpenBody body{};
		if (!m_session.NoteRampageStart(p->id, in.body, NowMs(), body))
			return;   // rule off: there is no session-wide rampage to be in

		const Session::Rampage &r = m_session.CurrentRampage();
		if (r.openedBy == p->id && body.elapsedMs == 0)
			Log(LogKind::Info,
			    "rampage %u open: %u kills in %d ms, started by %s%s", r.id,
			    r.target, r.limitMs, p->nick.c_str(),
			    r.target != in.body.target ? " (target scaled to the session)" : "");

		// To this one client and not broadcast. Every machine's own script
		// starts the frenzy by itself, so every machine sends its own start
		// and gets its own answer; broadcasting would hand the other seven a
		// kill count and an elapsed time they had not asked about, one per
		// player, for one rampage.
		S_RampageOpen out;
		InitHeader(out, NowMs());
		out.body = body;
		m_net.SendTo(peer, out, CH_EVENT);
	}

	void OnRampageKill(PeerId peer, const C_RampageKill &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.NoteRampageKill(in.body))
			return;

		// Everyone except them: their own engine counted it before this
		// packet was built, which is what produced it.
		S_RampageKill out;
		InitHeader(out, NowMs());
		out.byPlayer = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A car wreck, from the machine that decided it. Relayed the way a kill
	// is, except that a named car reported by a second machine is dropped
	// here: both copies of it blew up, but it's one car.
	void OnRampageCar(PeerId peer, const C_RampageCar &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.NoteRampageCar(in.body))
			return;

		S_RampageCar out;
		InitHeader(out, NowMs());
		out.byPlayer = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnRampageEnd(PeerId peer, const C_RampageEnd &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		RampageEndBody body{};
		if (!m_session.NoteRampageEnd(in.body, body))
			return;   // somebody already ended it, or it names an old frenzy

		Log(LogKind::Info, "rampage %u %s (%s got there first)", body.frenzyId,
		    body.outcome == RAMPAGE_PASSED ? "passed" : "failed", p->nick.c_str());
		BroadcastRampageEnd(body);
	}

	// To everybody including the reporter: a machine whose own CDarkel has
	// already ended still needs the verdict, because its rampage.sc is being
	// held on ONGOING until one arrives.
	void BroadcastRampageEnd(const RampageEndBody &body) {
		S_RampageEnd out;
		InitHeader(out, NowMs());
		out.body = body;
		m_net.Broadcast(out, CH_EVENT);
	}

	// ---- the vote before a rampage (rampagevote.h) ---------------------------

	uint32_t ActiveMask() const {
		uint32_t mask = 0;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (m_session.PlayerIdActive(id))
				mask |= 1u << id;
		return mask;
	}

	void DenyPickup(PeerId peer, const PickupIdent &ident) {
		S_PickupDenied out;
		InitHeader(out, NowMs());
		out.ident = ident;
		m_net.SendTo(peer, out, CH_EVENT);
	}

	void BroadcastVote(uint8_t state, uint32_t now) {
		S_RampageVote out;
		InitHeader(out, now);
		out.body = m_vote.Body(state, now);
		m_net.Broadcast(out, CH_EVENT);
	}

	// True when the claim was dealt with here: a vote opened, or the claim
	// was turned down because one is running, or ignored because it is the
	// toucher asking again while his own vote runs. False lets it through to
	// the ordinary grant.
	bool ClaimGoesToAVote(PeerId peer, const Player &p, const PickupIdent &ident) {
		if ((ident.flags & PICKUP_F_RAMPAGE) == 0 ||
		    !RampageNeedsVote(m_session.RampageRuleValue(), m_session.Count()))
			return false;

		const uint32_t now = NowMs();
		if (m_vote.IsOpen()) {
			if (m_vote.Starter() == p.id && Session::SameIdent(m_vote.Ident(), ident))
				return true;
			DenyPickup(peer, ident);
			return true;
		}
		// One running already. Its machine's engine refuses a second skull
		// anyway (CDarkel::FrenzyOnGoing in the pickup's gate); this is for the
		// machine whose own frenzy ended before the session's did.
		if (m_session.CurrentRampage().open) {
			DenyPickup(peer, ident);
			return true;
		}
		// Held for the toucher while everybody decides, so nobody else can
		// take it in the meantime. Denied here means somebody already has.
		if (m_session.ClaimPickup(p.id, ident, now) == Session::PickupVerdict::DENIED) {
			DenyPickup(peer, ident);
			return true;
		}

		m_vote.Start(p.id, ident, ActiveMask(), now);
		Log(LogKind::Info,
		    "rampage vote %u open: %s wants to start a rampage at (%.0f %.0f %.0f), "
		    "%u of %u have to say yes, %u s",
		    m_vote.Id(), p.nick.c_str(), ident.pos.x, ident.pos.y, ident.pos.z,
		    m_vote.Needed(), m_vote.Voters(), static_cast<unsigned>(RAMPAGE_VOTE_MS / 1000));
		BroadcastVote(RAMPAGE_VOTE_OPEN, now);
		m_vote.TakeDirty();
		// Two players and a skull: the toucher's yes may be all it takes if
		// the other one is gone by now. Decided here rather than a tick late.
		TickRampageVote(now);
		return true;
	}

	void OnRampageVote(PeerId peer, const C_RampageVote &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_vote.Cast(p->id, in.voteId, in.yes != 0))
			return;
		Log(LogKind::Detail, "rampage vote %u: %s says %s (%u yes, %u no, %u needed)",
		    m_vote.Id(), p->nick.c_str(), in.yes ? "yes" : "no", m_vote.Yes(), m_vote.No(),
		    m_vote.Needed());
		TickRampageVote(NowMs());
	}

	void OnRampageArrived(PeerId peer, const C_RampageArrived &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const char *what = "stayed where they were";
		switch (in.result) {
		case RAMPAGE_ARRIVED:          what = "was brought over"; break;
		case RAMPAGE_ARRIVED_LOCKED:   what = "was brought over, onto an island their story hasn't opened"; break;
		case RAMPAGE_SKIPPED_DEAD:     what = "stayed where they were: dead"; break;
		case RAMPAGE_SKIPPED_ARRESTED: what = "stayed where they were: being arrested"; break;
		case RAMPAGE_SKIPPED_CUTSCENE: what = "stayed where they were: in a cutscene"; break;
		case RAMPAGE_SKIPPED_MISSION:  what = "stayed where they were: on a mission"; break;
		case RAMPAGE_SKIPPED_NO_PED:   what = "stayed where they were: no player in the world"; break;
		default: break;
		}
		Log(LogKind::Detail, "rampage vote %u: %s %s", in.voteId, p->nick.c_str(), what);
	}

	// ---- skipping a cutscene together (cutscenevote.h) ------------------------

	static std::string CutsceneName(const CutsceneKey &key) {
		char name[CUTSCENE_NAME_LEN + 1] = {};
		for (size_t i = 0; i < CUTSCENE_NAME_LEN && key.name[i] != '\0'; ++i)
			name[i] = key.name[i] >= ' ' && key.name[i] <= '~' ? key.name[i] : '?';
		return name;
	}

	static const char *CutsceneScopeName(uint8_t scope) {
		return scope == CUTSCENE_SCOPE_SHARED ? "the mission's" : "their own";
	}

	void OnCutsceneState(PeerId peer, const C_CutsceneState &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const bool        was  = m_cutscenes.In(p->id);
		const CutsceneKey prev = m_cutscenes.KeyOf(p->id);
		const uint32_t    now  = NowMs();
		m_cutscenes.Report(p->id, in.key, now);
		// A press of skip comes in the same scene, and says nothing new here.
		const bool moved = !was || !SameCutscene(prev, in.key);
		if (moved && in.key.scope != CUTSCENE_SCOPE_NONE)
			Log(LogKind::Detail, "cutscene '%s': %s is in it (%s), %u player(s) in it",
			    CutsceneName(in.key).c_str(), p->nick.c_str(), CutsceneScopeName(in.key.scope),
			    m_cutscenes.VotersWith(p->id));
		else if (was && in.key.scope == CUTSCENE_SCOPE_NONE)
			Log(LogKind::Detail, "cutscene: %s is out of theirs", p->nick.c_str());
		if (in.skip)
			CutsceneSkipPressed(*p, in.voteId);
		FlushCutsceneVotes(now);
	}

	void CutsceneSkipPressed(const Player &player, uint8_t voteId) {
		const Player *p = &player;
		if (!m_cutscenes.Cast(p->id, voteId)) {
			Log(LogKind::Detail, "cutscene: a skip from %s for vote %u that isn't theirs to cast",
			    p->nick.c_str(), voteId);
			return;
		}
		Log(LogKind::Info, "cutscene '%s': %s votes to skip it",
		    CutsceneName(m_cutscenes.KeyOf(p->id)).c_str(), p->nick.c_str());
	}

	// Whatever the count decided, to whoever it is for.
	void FlushCutsceneVotes(uint32_t now) {
		CutsceneVoteSend sends[CutsceneVotes::MAX_SENDS];
		const size_t     n = m_cutscenes.Evaluate(now, sends, CutsceneVotes::MAX_SENDS);
		for (size_t i = 0; i < n; ++i) {
			const CutsceneVoteSend &e = sends[i];
			uint8_t sent = 0;
			for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
				if ((e.to >> id & 1u) == 0)
					continue;
				const Player *p = m_session.FindById(id);
				if (!p)
					continue;
				S_CutsceneVote out;
				InitHeader(out, now);
				out.kind = e.kind == CutsceneVoteSend::SKIP ? CUTSCENE_VOTE_SKIP : CUTSCENE_VOTE_COUNT;
				out.body = e.body;
				m_net.SendTo(p->peer, out, CH_EVENT);
				++sent;
			}
			if (e.kind == CutsceneVoteSend::SKIP) {
				if (e.body.voters != 0)
					Log(LogKind::Info,
					    "cutscene '%s' skipped for %u player(s): %u of %u said skip, %u needed",
					    CutsceneName(e.body.key).c_str(), sent, e.body.yes, e.body.voters,
					    e.body.needed);
				else
					Log(LogKind::Info,
					    "cutscene '%s': %u player(s) came into it after it was skipped, and skip "
					    "it too", CutsceneName(e.body.key).c_str(), sent);
			} else {
				Log(LogKind::Detail, "cutscene '%s': %u of %u say skip, %u needed",
				    CutsceneName(e.body.key).c_str(), e.body.yes, e.body.voters, e.body.needed);
			}
		}
	}

	// Decides the vote if it can, and says so. Called on every tick and on
	// anything that changes the count - a vote, a death, a leave.
	void TickRampageVote(uint32_t now) {
		const RampageVote::Outcome outcome = m_vote.Evaluate(ActiveMask(), now);
		if (outcome == RampageVote::Outcome::NONE)
			return;
		if (outcome == RampageVote::Outcome::OPEN) {
			if (m_vote.TakeDirty())
				BroadcastVote(RAMPAGE_VOTE_OPEN, now);
			return;
		}

		const uint8_t      state   = RampageVoteStateOf(outcome);
		const PickupIdent &ident   = m_vote.Ident();
		Player            *starter = m_session.FindById(m_vote.Starter());
		const RampageVoteBody end  = m_vote.Body(state, now);

		BroadcastVote(state, now);

		if (outcome != RampageVote::Outcome::PASSED) {
			Log(LogKind::Info, "rampage vote %u %s (%u of %u said yes, %u needed)", end.voteId,
			    outcome == RampageVote::Outcome::CANCELLED ? "called off, the one who started it is dead or gone"
			    : outcome == RampageVote::Outcome::FAILED_TIME ? "ran out of time"
			                                                   : "failed, yes can't get there any more",
			    end.yes, end.voters, end.needed);
			// The skull is back to how it was. The toucher's claim ends with
			// a denial, and his machine waits a moment before it asks again.
			m_session.ReleasePickup(ident, m_vote.Starter());
			if (starter)
				DenyPickup(starter->peer, ident);
			return;
		}

		if (!starter)
			return;   // can't happen: PASSED needs the starter present

		// The toucher takes it. The grant says it came out of a vote, which is
		// what lets his machine take it even if he has wandered off it.
		S_PickupGrant grant;
		InitHeader(grant, now);
		grant.ident = ident;
		grant.ident.flags |= PICKUP_F_VOTED;
		m_net.SendTo(starter->peer, grant, CH_EVENT);

		// And everybody else comes to him. Each machine moves its own player,
		// or doesn't and says why.
		const Vec3 at    = starter->havePos ? starter->pos : ident.pos;
		uint8_t    count = 0;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (id != starter->id && m_session.PlayerIdActive(id))
				++count;
		uint8_t slot = 0;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
			if (id == starter->id)
				continue;
			const Player *other = m_session.FindById(id);
			if (!other)
				continue;
			S_RampageTeleport out;
			InitHeader(out, now);
			out.body.voteId    = end.voteId;
			out.body.starterId = starter->id;
			out.body.slot      = slot++;
			out.body.count     = count;
			out.body.pos       = at;
			m_net.SendTo(other->peer, out, CH_EVENT);
		}

		Log(LogKind::Info,
		    "rampage vote %u passed (%u of %u said yes) - %s takes the skull, %u player(s) "
		    "sent to (%.0f %.0f %.0f)",
		    end.voteId, end.yes, end.voters, starter->nick.c_str(), count, at.x, at.y, at.z);
	}

	// A pedestrian somebody hosts has died and left something behind.
	// docs/pickups.md 10.
	//
	// **The server decides nothing here and keeps no record.** This is the
	// one pickup message that is a statement of fact rather than a request:
	// the pickup already exists in the sender's world, made by their engine
	// out of their own CGeneral::GetRandomNumber, and nobody else could have
	// worked out the numbers. So it is relayed and that is all.
	//
	// No table entry either, and that is measured rather than lazy.
	// CPickups::GenerateNewOne stamps a drop with an absolute expiry when it
	// makes one - 20 s for a weapon, 30 s for money (addresses.h, the ped
	// drop block) - so every copy dies on its own clock within half a minute
	// and there is nothing a late joiner could usefully be told. The moment
	// somebody claims one it enters m_pickups like any other key, because the
	// ident is a position and a model and has never cared who made it.
	//
	// It is also released back when the sender's own release arrives for the
	// same key, and it is denied to a second claimant by exactly the same
	// code as a shotgun in Ammu-Nation. Nothing about a drop is special once
	// it exists.
	void OnPickupDrop(PeerId peer, const C_PickupDrop &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		// A drop whose key is already held by somebody is a collision between
		// a fresh pickup and a live reservation. Dropping it is wrong - the
		// pickup does exist on the sender's machine - and so is overwriting
		// the reservation. Letting it through is what the clients' own
		// FindSlot collision check is for, and it logs there.
		S_PickupDrop out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// A map object broke on somebody's machine. docs/objects.md.
	//
	// Relayed, and kept while a player is near it. The engine throws a break
	// away at 80 m - CPopulation::ManagePopulation turns any map object that
	// far from *its own* player back into a pristine dummy - and that horizon
	// is per machine: a player who drove off and came back had his copy
	// rebuilt standing while one who stayed still saw it broken. So the
	// record lives exactly as long as somebody may still hold it broken
	// (objectrecords.h), and a machine that rebuilds it asks. Exclusivity does
	// not arise: two players can both correctly break the same crate and the
	// second break is a no-op on every machine, which is the opposite of a
	// pickup.
	//
	// The client decides who is entitled to speak, because only the client
	// knows which car hit it. What the server does is exactly what it does
	// for a ped drop: stamp it with the sender and pass it on.
	void OnObjectBroken(PeerId peer, const C_ObjectBroken &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_ObjectBroken out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
		// And kept while anybody is near it, for a machine that rebuilds its
		// copy pristine after going 80 m away (objectrecords.h).
		m_objects.NoteBroken(p->id, in.body, NowMs());
	}

	// A machine rebuilt an object it was told was broken: the break and the
	// resting place again, to it alone. Nothing when the record has gone,
	// which means nobody has been near it and every copy is pristine.
	void OnObjectRebuilt(PeerId peer, const C_ObjectRebuilt &in) {
		if (!m_session.FindByPeer(peer))
			return;
		const ObjectRecord *r = m_objects.Find(in.ident);
		if (!r)
			return;
		SendObjectRecord(peer, *r);
	}

	// Stamped with nobody rather than the reporter: the asker may be the
	// reporter, whose client drops its own relayed break, and here it is
	// exactly the one who needs it back.
	void SendObjectRecord(PeerId peer, const ObjectRecord &r) {
		S_ObjectBroken out;
		InitHeader(out, NowMs());
		out.playerId = INVALID_PLAYER;
		out.body     = r.breakBody;
		m_net.SendTo(peer, out, CH_EVENT);
		if (!r.hasRest)
			return;
		S_ObjectSettled rest;
		InitHeader(rest, NowMs());
		rest.playerId = INVALID_PLAYER;
		rest.body     = r.rest;
		m_net.SendTo(peer, rest, CH_EVENT);
	}

	// The records nobody is near any more, once a second. Every machine's
	// copy of those has gone back to a dummy, so they come back pristine
	// everywhere and the record means nothing.
	void ExpireObjectRecords(uint32_t now) {
		if (now - m_objectsExpiredMs < 1000)
			return;
		m_objectsExpiredMs = now;
		Vec3   where[MAX_PLAYERS];
		size_t n = 0;
		for (const Player &q : m_session.Players())
			if (q.active && q.havePos && n < MAX_PLAYERS)
				where[n++] = q.pos;
		m_objects.ExpireFar(where, n);
	}

	// ---- the scripted gates (protocol.h, C_GateState) ----------------------
	//
	// The garages' shape exactly: a level per player, relayed on change, and
	// kept only so a joiner can be told.
	void OnGateState(PeerId peer, const C_GateState &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const uint8_t mask = static_cast<uint8_t>(in.body.open & ((1u << GATE_COUNT) - 1));
		if (mask == p->gateMask)
			return;
		p->gateMask = mask;
		S_GateState out{};
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId  = p->id;
		out.body.open = mask;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	// ---- cheats - docs/cheats.md -------------------------------------------
	//
	// A cheat that changes something its typist's machine does not own. The
	// decision is Session::NoteCheat's, which is CheatRelayFor over the rule
	// and the host: a sky to the host alone, the rest to everybody but the
	// typist, whose own engine already ran it. Everything else is dropped -
	// a personal cheat never comes here at all, and one the rule refuses is
	// refused a second time here for a client too old to have refused it.
	void OnCheat(PeerId peer, const C_Cheat &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_Cheat out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;

		switch (m_session.NoteCheat(p->id, in.body)) {
		case CHEAT_RELAY_HOST: {
			const Player *sky = m_session.FindById(m_session.SkyHolderId());
			if (!sky)
				return;
			m_net.SendTo(sky->peer, out, CH_EVENT);
			Log(LogKind::Detail, "%s used cheat %u; sent to %s, whose sky it is",
			    p->nick.c_str(), static_cast<unsigned>(in.body.cheat),
			    sky->nick.c_str());
			return;
		}
		case CHEAT_RELAY_OTHERS:
			m_net.Broadcast(out, CH_EVENT, peer);
			Log(LogKind::Detail, "%s used cheat %u; everybody runs it (state %u)",
			    p->nick.c_str(), static_cast<unsigned>(in.body.cheat),
			    static_cast<unsigned>(in.body.state));
			return;
		default:
			Log(LogKind::Warn, "%s used cheat %u, which this session does not "
			    "pass on (cheats = %s)", p->nick.c_str(),
			    static_cast<unsigned>(in.body.cheat),
			    m_session.CheatRuleValue() == CHEAT_RULE_OFF        ? "off"
			    : m_session.CheatRuleValue() == CHEAT_RULE_PERSONAL ? "personal"
			                                                        : "shared");
			return;
		}
	}

	// ---- a unique jump's shot - stuntcam.h -----------------------------------
	//
	// To the players riding in the car, from its driver. Nothing is kept: a
	// shot is a few seconds long, and somebody who gets in halfway through
	// sees the rest of the jump from behind.
	void OnStuntCamera(PeerId peer, const C_StuntCamera &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!StuntShotSenderOk(*p, in.body)) {
			Log(LogKind::Detail, "%s sent a stunt shot of vehicle %u from outside its "
			    "driver's seat; dropped", p->nick.c_str(), static_cast<unsigned>(in.body.netId));
			return;
		}
		S_StuntCamera out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;
		const std::vector<uint8_t> to = StuntShotAudience(m_session.Players(), *p, in.body);
		for (uint8_t id : to)
			if (const Player *q = m_session.FindById(id))
				m_net.SendTo(q->peer, out, CH_EVENT);
		if (in.body.on && !to.empty())
			Log(LogKind::Detail, "%s is in a unique jump in vehicle %u; the shot goes to %u "
			    "riding with him", p->nick.c_str(), static_cast<unsigned>(in.body.netId),
			    static_cast<unsigned>(to.size()));
	}

	// ---- money - protocol.h, MoneyRule ---------------------------------------
	//
	// Under `shared` a player's cash moved. The server adds it up and tells
	// everybody the total, each with their own ack, so a machine with changes
	// still in flight doesn't write them away.
	void OnMoneyChange(PeerId peer, const C_MoneyChange &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		const bool seeds = !m_session.MoneyPoolSeeded();
		if (!m_session.NoteMoneyChange(p->id, in.body))
			return;

		if (seeds)
			Log(LogKind::Detail, "%s brought $%d, which is now everybody's",
			    p->nick.c_str(), m_session.MoneyPool());
		else if (in.body.delta != 0)
			Log(LogKind::Detail, "%s: $%+d, the session has $%d", p->nick.c_str(),
			    in.body.delta, m_session.MoneyPool());

		for (const Player &q : m_session.Players())
			if (q.active)
				m_net.SendTo(q.peer,
				             m_session.MoneyFor(q.id, p->id, in.body.delta, NowMs()),
				             CH_EVENT);
	}

	// A wreck the sender decided and somebody else earned - or the sender
	// earned, on a car other machines decide too. To the recipient alone.
	void OnMoneyAward(PeerId peer, const C_MoneyAward &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		if (!m_session.TakeMoneyAward(p->id, in.body, NowMs()))
			return;
		const Player *to = m_session.FindById(in.body.toPlayerId);
		if (!to)
			return;

		S_MoneyAward out;
		InitHeader(out, NowMs());
		out.fromPlayerId = p->id;
		out.body         = in.body;
		m_net.SendTo(to->peer, out, CH_EVENT);
		Log(LogKind::Detail, "%s's game sends %s $%d for a wrecked car",
		    p->nick.c_str(), to->nick.c_str(), in.body.unit);
	}

	// Where a knocked-over one came to rest. Same job and same reasons:
	// stamp the sender, pass it on, and keep it beside the break for a
	// machine that builds its copy again (objectrecords.h).
	void OnObjectSettled(PeerId peer, const C_ObjectSettled &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_ObjectSettled out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
		m_objects.NoteSettled(p->id, in.body, NowMs());
	}

	void OnPickupRelease(PeerId peer, const C_PickupRelease &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;
		// Deliberately not restricted to the holder. The second meaning of a
		// release is "the script has re-created this one", and the client that
		// notices that is any client whose engine has a live object at a key
		// it had previously removed - which is everybody except the one who
		// collected it. docs/pickups.md 7.
		m_session.ReleasePickup(in.ident, p->id);
	}

	void OnWorldState(PeerId peer, const C_WorldState &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p || p->id != m_session.SkyHolderId())
			return;   // not the host, or not the running mission's owner

		if (!m_session.Clock().Set(in.body.hour, in.body.minute))
			return;
		m_session.SetWeather(in.body.weather, in.body.weatherOld);

		m_lastWorldMs = NowMs();
		BroadcastWorldState();
	}

	// Where somebody's machine has everybody else, against where everybody
	// said they were at that instant. protocol.h, C_DesyncProbe. The answer goes
	// back to the prober alone; the log gets the worst row of a probe that has
	// one past DESYNC_LOG_M, at most once every DESYNC_LOG_EVERY_MS a prober.
	void OnDesyncProbe(PeerId peer, const C_DesyncProbe &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		const uint8_t count = in.count < DESYNC_PROBE_ROWS ? in.count : DESYNC_PROBE_ROWS;
		S_DesyncReport out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.count = count;

		uint8_t  compared = 0, over = 0;
		uint16_t worstCm = 0, worstNetId = INVALID_NETID;
		uint8_t  worstOwner = INVALID_PLAYER;
		for (uint8_t i = 0; i < count; ++i) {
			const DesyncProbeRow &row = in.rows[i];
			out.rows[i].netId = row.netId;
			out.rows[i].offCm = DESYNC_UNKNOWN;
			const PoseHistory *history = m_session.HistoryFor(row.netId, p->id);
			if (!history)
				continue;
			const uint16_t cm = DesyncOffCm(*history, row);
			out.rows[i].offCm = cm;
			if (cm == DESYNC_UNKNOWN)
				continue;
			++compared;
			if (cm >= static_cast<uint16_t>(DESYNC_LOG_M * 100.0f))
				++over;
			if (worstNetId == INVALID_NETID || cm > worstCm) {
				worstCm    = cm;
				worstNetId = row.netId;
				worstOwner = history->Reporter();
			}
		}
		// Unreliable, like the probe: the next one is two seconds away.
		m_net.SendTo(peer, out, CH_SNAPSHOT);

		if (!p->saidDesync && compared > 0) {
			p->saidDesync = true;
			Log(LogKind::Info, "answered %s's first desync probe: %u of %u of their "
			                   "copies compared, the furthest %.1f m out",
			    p->nick.c_str(), compared, count, worstCm / 100.0f);
		}
		if (over == 0)
			return;
		const uint32_t now = NowMs();
		if (p->desyncSaidAtMs != 0 && now - p->desyncSaidAtMs < DESYNC_LOG_EVERY_MS)
			return;
		p->desyncSaidAtMs = now ? now : 1;

		char more[64] = "";
		if (over > 1)
			std::snprintf(more, sizeof more, " (and %u more past %.0f m in the same probe)",
			              static_cast<unsigned>(over - 1), static_cast<double>(DESYNC_LOG_M));
		const Player *whose = m_session.FindByNetId(worstNetId);
		if (whose) {
			Log(LogKind::Warn, "desync: %s has %s %.1f m from where %s was at that "
			                   "instant%s", p->nick.c_str(), whose->nick.c_str(),
			    worstCm / 100.0f, whose->nick.c_str(), more);
			return;
		}
		const Player *owner = m_session.FindById(worstOwner);
		const char   *what  = m_session.FindCar(worstNetId)   ? "traffic car"
		                      : m_session.FindPed(worstNetId) ? "pedestrian"
		                                                      : "vehicle";
		Log(LogKind::Warn, "desync: %s has %s %u %.1f m from where %s had it at "
		                   "that instant%s", p->nick.c_str(), what, worstNetId,
		    worstCm / 100.0f, owner ? owner->nick.c_str() : "its last reporter", more);
	}

	// Everybody's round trip, for the player list. On its own timer rather than
	// the world packet's, which the host's own reports keep resetting.
	void BroadcastPings(uint32_t now) {
		if (m_session.Count() == 0)
			return;
		S_PlayerPings out{};
		InitHeader(out, now);
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
			const Player *p = m_session.FindById(id);
			if (!p || !p->active) {
				out.rttMs[id] = PING_NONE;
				continue;
			}
			const uint32_t rtt = m_net.RoundTripMs(p->peer);
			out.rttMs[id]      = static_cast<uint16_t>(rtt < PING_MAX ? rtt : PING_MAX);
		}
		// Unreliable: a lost one is the same number a second later.
		m_net.Broadcast(out, CH_SNAPSHOT);
	}

	void BroadcastWorldState() {
		if (m_session.Count() == 0)
			return;

		S_WorldState out;
		// Our own clock, never the host's C_WorldState header. Clients run
		// the trains and planes off this timestamp (client/src/sessiontime.h),
		// so it has to be the same clock for every one of them.
		InitHeader(out, NowMs());
		out.body.hour       = m_session.Clock().Hour();
		out.body.minute     = m_session.Clock().Minute();
		out.body.weather    = m_session.Weather();
		out.body.weatherOld = m_session.WeatherOld();
		out.hostPlayerId    = m_session.HostId();
		m_net.Broadcast(out, CH_EVENT);
	}

public:
	// ---- what a front end needs -------------------------------------------
	//
	// The console server prints these lines and the window puts them in its
	// console panel. Same text either way: the log is the server's account of
	// itself, and two accounts that differ would be two servers.
	using LogSink = std::function<void(LogKind kind, const char *line)>;

	void SetLogSink(LogSink sink) { m_log = std::move(sink); }

	Session       &SessionRef() { return m_session; }
	const Session &SessionRef() const { return m_session; }
	const NetServer &Net() const { return m_net; }
	const Lobby   &LobbyRef() const { return m_lobby; }

	uint16_t Port() const { return m_port; }

	// ENet's estimate of the round trip to a player, in ms. The window shows
	// it per row and turns it amber past 150.
	uint32_t PingOf(const Player &player) const { return m_net.RoundTripMs(player.peer); }
	bool     Listening() const { return m_listening; }

	// Milliseconds since Start(), for the header's "up 1:12:40".
	uint32_t UptimeMs() const { return m_listening ? NowMs() - m_startedMs : 0; }

	// Throws a player out. LEAVE_KICKED is the reason everyone else is given,
	// so a kick reads differently from a timeout in every client's log.
	bool Kick(uint8_t playerId) {
		Player *p = m_session.FindById(playerId);
		if (!p)
			return false;
		const PeerId peer = p->peer;
		RemovePlayer(peer, LEAVE_KICKED);
		// With the reason, which a client reads as "do not come straight
		// back"; the ENet event that follows finds nobody left to remove.
		m_net.Disconnect(peer, LEAVE_KICKED);
		return true;
	}

private:
	// Formats one line and hands it to the sink, which decides where it goes
	// and what the tag column looks like. Callers write the message, not the
	// furniture.
	void Log(LogKind kind, const char *fmt, ...) {
		char    line[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(line, sizeof(line), fmt, args);
		va_end(args);

		if (m_log)
			m_log(kind, line);
		else
			std::printf("%s %s\n", TagOf(kind), line);
	}

	LogSink  m_log;
	bool     m_saidCarDamage = false;
	bool     m_saidOwnPackage = false;
	uint16_t m_port      = DEFAULT_PORT;
	bool     m_listening = false;
	uint32_t m_startedMs = 0;

	NetServer                m_net;
	Session                  m_session;
	// Broken and knocked-over street objects while anybody is near them.
	ObjectRecords            m_objects;
	uint32_t                 m_objectsExpiredMs = 0;
	Lobby                    m_lobby;
	std::vector<ServerEvent> m_events;
	uint32_t                 m_lastTickMs  = 0;
	uint32_t                 m_lastWorldMs = 0;
	uint32_t                 m_lastPingsMs = 0;
	uint8_t                  m_skyHolder   = INVALID_PLAYER;   // as last logged
	// The session's Import/Export and crane lists (OnCarLists), for as long
	// as the server runs, like the campaign log.
	uint32_t                 m_carLists[CAR_LISTS] = {};

	// The vote before a rampage. rampagevote.h has the rules.
	RampageVote m_vote;
	// Who is in which cutscene, and who said skip. cutscenevote.h.
	CutsceneVotes m_cutscenes;

	std::string               m_password;
	std::vector<PendingHello> m_pendingHellos;
	// When each connection came up, while it has not yet said hello; 0 for
	// none. Bit 0 is forced on so a connection at 0 ms still counts.
	uint32_t m_connectedAtMs[SERVER_PEERS] = {};
};

} // namespace coopiii
