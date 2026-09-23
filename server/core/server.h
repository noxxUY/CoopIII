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

#include "session.h"

#include "coopiii/net.h"

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

class Server {
public:
	// ammoSync and wantedRule are defaulted rather than required so the two
	// front ends that already call this keep compiling; both pass both.
	bool Start(uint16_t port, bool friendlyFire, bool ammoSync = false,
	           uint8_t wantedRule  = WANTED_RULE_PERPLAYER,
	           uint8_t rampageRule = RAMPAGE_RULE_SHARED,
	           uint8_t cheatRule   = CHEAT_RULE_SHARED,
	           uint8_t moneyRule   = MONEY_RULE_OFF) {
		if (!NetInit()) {
			Log(LogKind::Warn, "enet init failed");
			return false;
		}
		if (!m_net.Listen(port, MAX_PLAYERS)) {
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
		m_port      = port;
		m_listening = true;
		m_startedMs = NowMs();
		Log(LogKind::Info, "listening on %u, %u slots, friendly fire %s, "
		            "ammo sync %s, wanted level %s, money %s", port, MAX_PLAYERS,
		            friendlyFire ? "on" : "off", ammoSync ? "on" : "off",
		            m_session.WantedRule() == WANTED_RULE_SHARED ? "shared"
		                : m_session.WantedRule() == WANTED_RULE_OFF ? "off"
		                                                            : "per player",
		            m_session.MoneyRuleValue() == MONEY_RULE_SHARED ? "shared"
		                : m_session.MoneyRuleValue() == MONEY_RULE_OWN ? "own"
		                                                               : "off");
		return true;
	}

	void Stop() {
		m_listening = false;
		m_net.Shutdown();
		NetDeinit();
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

		m_events.clear();
		m_net.Service(m_events, waitMs);
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

	// Everybody, the claimer included. On the machine whose engine made the
	// car the client only forgets the row and leaves the car to its engine
	// (RemoteVehicle::ours); everywhere else the copy is destroyed. This is
	// the only sender of S_VehicleDespawn.
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

	void OnDisconnect(PeerId peer) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		Log(LogKind::Leave, "%s left (slot %u)", p->nick.c_str(), p->id);

		S_PlayerLeave out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.reason   = LEAVE_QUIT;

		// Anything they had reserved is free again at once. What they
		// actually collected stays collected: the respawn window runs on
		// whether or not they are still connected.
		m_session.ReleaseReservationsOf(p->id);

		const uint8_t wasHost = m_session.HostId();
		// Before RemovePeer, while the ped rows still say whose they are.
		DropPedsOf(p->id);
		DropCarsOf(p->id);
		m_session.RemovePeer(peer);
		m_net.Broadcast(out, CH_EVENT, peer);

		// The host walking out hands the clock to whoever is left. Everyone
		// finds out from the next world packet, at most a second away, which
		// is also when the new host starts reporting.
		if (wasHost == out.playerId && m_session.HostId() != INVALID_PLAYER) {
			const Player *host = m_session.FindById(m_session.HostId());
			Log(LogKind::Info, "the host left; %s has the clock now",
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
		case OP_C_CAR_SPAWN:
			if (const auto *pkt = msg.as<C_CarSpawn>())
				OnCarSpawn(peer, *pkt);
			break;
		case OP_C_CAR_DESPAWN:
			if (const auto *pkt = msg.as<C_CarDespawn>())
				OnCarDespawn(peer, *pkt);
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
		case OP_C_OBJECT_SETTLED:
			if (const auto *pkt = msg.as<C_ObjectSettled>())
				OnObjectSettled(peer, *pkt);
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
		const AmbientPed *ped = m_session.FindPed(in.body.netId);
		if (!ped || ped->ownerPlayerId != p->id)
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
			out.peds[out.count++] = in.peds[i];
		}

		if (out.count == 0)
			return;
		m_net.Broadcast(out, CH_SNAPSHOT, peer);
	}

	// Everything `playerId` was hosting goes with them. Called before
	// Session::RemovePeer, while the rows still say whose they are.
	void DropPedsOf(uint8_t playerId) {
		const std::vector<uint16_t> owned = m_session.PedsOwnedBy(playerId);
		for (uint16_t netId : owned) {
			m_session.RemovePed(netId, INVALID_PLAYER);
			S_PedDespawn out;
			InitHeader(out, NowMs());
			out.netId = netId;
			m_net.Broadcast(out, CH_EVENT);
		}
		if (!owned.empty())
			Log(LogKind::Detail, "slot %u took %zu ambient ped(s) with them",
			            playerId, owned.size());
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
				std::printf("[coopiii] %s claimed a car with temp id 0; "
				            "ignoring (and not saying so again)\n",
				            p->nick.c_str());
			}
			return;
		}

		AmbientCar *car = m_session.AddCar(p->id, in.body);
		if (!car) {
			if (!p->warnedCarCap) {
				p->warnedCarCap = true;
				std::printf("[coopiii] the session is full of ambient cars "
				            "(%zu); refusing %s's claims from here on\n",
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

	// Everything `playerId` was hosting goes with them, cars as well as peds.
	void DropCarsOf(uint8_t playerId) {
		const std::vector<uint16_t> owned = m_session.CarsOwnedBy(playerId);
		for (uint16_t netId : owned) {
			m_session.RemoveCar(netId, INVALID_PLAYER);
			S_CarDespawn out;
			InitHeader(out, NowMs());
			out.netId = netId;
			m_net.Broadcast(out, CH_EVENT);
		}
		if (!owned.empty())
			std::printf("[coopiii] slot %u took %zu ambient car(s) with them\n",
			            playerId, owned.size());
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
			Log(LogKind::Warn, "rejected peer %u: %s", peer, RejectText(reject));
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
		for (const S_PlayerAmmo &ammo : back.ammo)
			m_net.SendTo(peer, ammo, CH_EVENT);
		for (const S_VehicleSpawn &spawn : back.vehicles)
			m_net.SendTo(peer, spawn, CH_EVENT);
		for (const S_EnterVehicle &seat : back.seats)
			m_net.SendTo(peer, seat, CH_EVENT);
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
		// And which doors are currently open for somebody. Without it a
		// joiner is the one player whose safehouse door is shut while
		// somebody is standing inside it, until that somebody walks away -
		// at which point they would be told a door they never saw open had
		// closed.
		for (const S_GarageState &garage : back.garages)
			m_net.SendTo(peer, garage, CH_EVENT);
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
		m_net.Broadcast(m_session.MakeJoin(*p, NowMs()), CH_EVENT, peer);

		Log(LogKind::Join, "%s joined (slot %u, net %u); backfilled %zu player(s), "
		            "%zu vehicle(s), %zu seat(s)",
		            p->nick.c_str(), p->id, p->netId, back.players.size(),
		            back.vehicles.size(), back.seats.size());
		if (m_session.HostId() == p->id)
			Log(LogKind::Info, "%s is the host; the session's clock is theirs",
			            p->nick.c_str());
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
	void OnVehicleDamage(PeerId peer, const C_VehicleDamage &in) {
		Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		if (!m_session.MayReportVehicle(p->id, in.body.netId))
			return;   // OnVehicleState already says this out loud, once

		VehicleDamageBody merged{};
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
		// A wreck takes no more updates either: its driver is dead, so anything
		// still arriving for it was sampled before the blast.
		Vehicle *known = m_session.FindVehicle(in.body.netId);
		if (known && known->destroyed)
			return;
		m_session.NoteVehicleState(in.body);
		if (known && known->destroyed)
			Log(LogKind::Detail, "vehicle %u is wrecked; joiners will not be told "
			            "about it", known->netId);

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

			Log(LogKind::Detail, "vehicle %u claimed by %s (model %u, extras %d/%d)",
			            v->netId, p->nick.c_str(), v->modelId,
			            static_cast<int>(v->extra1), static_cast<int>(v->extra2));
		}

		// Every seat, not just the driver's. A passenger's seat has one
		// carrier on the wire and that is this packet, so a session that does
		// not write it down is a session that cannot tell the next joiner
		// about it - see Player::seat.
		const uint8_t displaced = m_session.NoteEnterVehicle(*p, *v, in.body.seat);
		p->warnedVehicleAuthority = false;

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
		if (v->driverPlayerId != p->id)
			return;

		v->destroyed      = true;
		v->driverPlayerId = INVALID_PLAYER;
		v->pos            = in.body.pos;
		v->rot            = in.body.rot;
		if (p->vehicleNetId == v->netId)
			p->vehicleNetId = INVALID_NETID;

		Log(LogKind::Detail, "vehicle %u blown up by %s", v->netId, p->nick.c_str());

		S_VehicleBlowUp out;
		InitHeader(out, in.hdr.sendTimeMs);
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
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

		if (m_session.ClaimPickup(p->id, in.ident, NowMs()) ==
		    Session::PickupVerdict::DENIED) {
			S_PickupDenied out;
			InitHeader(out, NowMs());
			out.ident = in.ident;
			m_net.SendTo(peer, out, CH_EVENT);
			return;
		}

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
	// A pure relay, and the absence of a table here is the design rather than
	// a gap in it. A break is a latch the engine itself throws away at 80 m -
	// CPopulation::ManagePopulation turns any map object that far from a
	// player back into a pristine dummy - so there is no session state to
	// keep and nothing a late joiner could be told that would still be true
	// by the time they finished loading. Exclusivity does not arise either:
	// two players can both correctly break the same crate and the second
	// break is a no-op on every machine, which is the opposite of a pickup.
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
			const Player *host = m_session.FindById(m_session.HostId());
			if (!host)
				return;
			m_net.SendTo(host->peer, out, CH_EVENT);
			Log(LogKind::Detail, "%s used cheat %u; sent to the host, %s",
			    p->nick.c_str(), static_cast<unsigned>(in.body.cheat),
			    host->nick.c_str());
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

	// Where a knocked-over one came to rest. Same job, same reasons, and
	// deliberately the same amount of server: stamp the sender and pass it
	// on. The server has no opinion about whether a lamp post fell over,
	// holds no table of the ones that did, and tells no joiner - the engine
	// converts the object back to a pristine dummy 80 m out and throws the
	// whole transform away, so anything remembered here would be a fact with
	// a shorter life than the packet that carried it.
	void OnObjectSettled(PeerId peer, const C_ObjectSettled &in) {
		const Player *p = m_session.FindByPeer(peer);
		if (!p)
			return;

		S_ObjectSettled out;
		InitHeader(out, NowMs());
		out.playerId = p->id;
		out.body     = in.body;
		m_net.Broadcast(out, CH_EVENT, peer);
	}

	void OnPickupRelease(PeerId peer, const C_PickupRelease &in) {
		if (!m_session.FindByPeer(peer))
			return;
		// Deliberately not restricted to the holder. The second meaning of a
		// release is "the script has re-created this one", and the client that
		// notices that is any client whose engine has a live object at a key
		// it had previously removed - which is everybody except the one who
		// collected it. docs/pickups.md 7.
		m_session.ReleasePickup(in.ident);
	}

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

		const PeerId    peer = p->peer;
		const std::string nick = p->nick;

		S_PlayerLeave out;
		InitHeader(out, NowMs());
		out.playerId = playerId;
		out.reason   = LEAVE_KICKED;
		m_net.Broadcast(out, CH_EVENT, peer);

		DropPedsOf(playerId);
		m_session.RemovePeer(peer);
		m_net.Disconnect(peer, LEAVE_KICKED);
		Log(LogKind::Leave, "%s was kicked (slot %u)", nick.c_str(), playerId);
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
	uint16_t m_port      = DEFAULT_PORT;
	bool     m_listening = false;
	uint32_t m_startedMs = 0;

	NetServer                m_net;
	Session                  m_session;
	std::vector<ServerEvent> m_events;
	uint32_t                 m_lastTickMs  = 0;
	uint32_t                 m_lastWorldMs = 0;
};

} // namespace coopiii
