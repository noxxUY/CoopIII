#include "client.h"

#include "game/adopt.h"
#include "game/horn.h"
#include "game/streampick.h"
#include "game/wanted.h"
#include "log.h"

#include <cmath>
#include <cstring>

namespace coopiii {

bool Client::Start(const std::string &host, uint16_t port, const std::string &nick,
                   const WorldBridge &bridge) {
	m_bridge = bridge;
	BindHelis();
	BindMoney();
	ClearRoster();
	m_localNick = nick;
	return m_net.Start(host, port, nick);
}

// Same reason as BindHelis, plus the roster for the one thing the engine half
// can only name by netId: the ped of the player who earned an award.
void Client::BindMoney() {
	m_money.Bind(
	    &m_bridge.money,
	    [](void *ctx, const void *bytes, size_t len, Channel ch) {
		    static_cast<NetThread *>(ctx)->SendRaw(bytes, len, ch);
	    },
	    &m_net,
	    [](void *ctx, uint16_t netId) -> uint8_t {
		    const Client *self = static_cast<const Client *>(ctx);
		    for (const RemotePlayer &p : self->m_players)
			    if (p.active && p.netId == netId)
				    return p.playerId;
		    return INVALID_PLAYER;
	    },
	    this);
}

// HeliSync reads the bridge through a pointer into m_bridge and sends through
// the socket thread, so it is bound again whenever m_bridge is assigned.
void Client::BindHelis() {
	m_helis.Bind(
	    &m_bridge.heli,
	    [](void *ctx, const void *bytes, size_t len, Channel ch) {
		    static_cast<NetThread *>(ctx)->SendRaw(bytes, len, ch);
	    },
	    &m_net);
}

void Client::Stop() {
	m_net.Stop();
	ClearRoster();
	ForgetRejoin();
}

uint8_t Client::RemoteCount() const {
	uint8_t n = 0;
	for (const RemotePlayer &p : m_players)
		if (p.active)
			++n;
	return n;
}

// The newest position each of them sent, or the one the session gave us at
// the join or the respawn. A player we have never been told a position for
// counts for nothing rather than for the world origin.
uint32_t Client::ViewerPositions(Vec3 *out, uint32_t max) const {
	uint32_t n = 0;
	for (const RemotePlayer &p : m_players) {
		if (n >= max)
			break;
		if (p.active && p.haveState && p.playerId != m_localPlayerId)
			out[n++] = p.last.pos;
	}
	return n;
}

// Takes a player out of whatever seat the engine has them in.
//
// Called before either half of the pair stops existing, and the ped has to
// leave first: a car destroyed under a seated ped leaves it with bInVehicle
// still set, and the only thing keeping m_pMyVehicle from dangling is the
// reference the engine registered - which nils the pointer but doesn't
// clear the flag. CPed::ProcessControl then goes and follows a null car.
void Client::UnseatPlayer(RemotePlayer &player) {
	// An entry that is still in flight has to come off first, and it has to
	// come off through the engine: the door it claimed is held in the car's
	// m_nGettingInFlags until QuitEnteringCar gives it back, and a door
	// nobody gives back is a door nobody can ever use again. This is why
	// every caller can go on calling UnseatPlayer and nothing else.
	if (player.Entering()) {
		if (m_bridge.AbandonSeatRemotePed)
			m_bridge.AbandonSeatRemotePed(player);
		player.enteringVehicleNetId = INVALID_NETID;
		player.enteringJack         = false;
	}
	if (!player.Seated())
		return;
	if (m_bridge.UnseatRemotePed)
		m_bridge.UnseatRemotePed(player);
	player.seatedVehicleNetId = INVALID_NETID;
}

void Client::ClearRoster() {
	m_localPlayerId       = 0xFF;
	m_localNetId          = INVALID_NETID;
	// A vote, or a move to somebody, from the old session means nothing here.
	m_rampageVote.Clear();
	m_teleportWaiting     = false;
	for (uint16_t &ping : m_pings)
		ping = PING_NONE;
	m_lastProbeMs    = 0;
	m_probeCarCursor = 0;
	// Limbs that came off with no session to tell. Left queued they would go
	// out under whatever netId the next welcome gives us.
	if (m_bridge.DrainAmbientBodyParts) {
		PedBodyPartBody stale[16];
		while (m_bridge.DrainAmbientBodyParts(stale, 16) != 0) {
		}
	}
	// Nobody's clock is ours to follow once the session is gone. The sky
	// stays pinned where the last host left it, deliberately: unpinning it
	// here would change the weather on a player who has just been
	// disconnected and is about to reconnect to the same session.
	m_hostPlayerId        = INVALID_PLAYER;
	// The trains and planes go back to CTimer's clock until a server says
	// otherwise. A reconnect can land on a different server, whose clock
	// started at a different moment, so the old estimate is worth nothing
	// there.
	m_sessionTime.Reset();
	m_localVehicleNetId   = INVALID_NETID;
	m_vehicleClaimPending = false;
	m_claimRetryAtMs      = 0;
	// A new session hasn't been told anything about us yet.
	m_sentModelId         = 0xFFFF;
	std::memset(m_sentLook, 0, sizeof m_sentLook);
	// Nor has it been told we died. A death announced to the old session is
	// not a death this one knows about, and leaving the flag set would mean
	// the first respawn after a reconnect gets announced on its own.
	m_deathAnnounced      = false;
	// Same for an arrest. If we are still in PED_ARRESTED the next sample
	// notes it again, against the new session.
	m_arrestNoted         = false;
	m_lastAttackerNetId   = INVALID_NETID;
	m_lastAttackerMs      = 0;
	// The wanted bookkeeping is about a session, so it goes with one, and the
	// rule goes back to the default - a client with no session must not go on
	// clamping its own player to zero because the server it has just lost had
	// the wanted level switched off.
	m_wantedRule          = WANTED_RULE_PERPLAYER;
	// A rampage belongs to a session, so it goes with one. The rule goes back
	// to §5.10's default and the seam is told, which puts this machine's
	// rampage.sc back in touch with its own CDarkel - a script held waiting
	// on a verdict from a server that has gone would otherwise sit in that
	// loop with $ONMISSION set for the rest of the game.
	m_rampageRule         = RAMPAGE_RULE_SHARED;
	m_frenzyId            = NO_FRENZY;
	m_frenzyReported      = false;
	if (m_bridge.ResetRampage)
		m_bridge.ResetRampage();
	// Cheats go back to single player's: the key handler runs the engine's
	// own function again from the next keystroke. Told now rather than on the
	// next PreFrame, because a keystroke can arrive before it.
	//
	// What a session's cheats left behind stays. A riot, a time scale, armed
	// pedestrians - single player keeps every one of them until a load runs
	// CPad::ResetCheats, and a dropped connection is not a load.
	m_cheatRule           = CHEAT_RULE_SHARED;
	m_worldSendNow        = false;
	if (m_bridge.SetCheatSession)
		m_bridge.SetCheatSession(false, false, CHEAT_RULE_SHARED);
	// Money goes back to off, which is every machine paying its own player.
	// The cash is left alone, a shared session's included: dropping out is
	// not a reason to lose it, and putting the old wallet back would need a
	// copy nobody has kept.
	m_money.Clear();
	m_helis.SetShootDownPaid(false);
	// The engine's stars are deliberately *not* touched. Whatever this
	// player has, they earned or adopted, and it is theirs - clearing it on a
	// disconnect would mean a dropped connection is a get-out-of-jail card,
	// and re-applying it on reconnect would need state that has just been
	// thrown away. Zeroing `applied` is what makes the next tick read the
	// engine's level as this player's own, which it now is.
	m_ownWanted           = 0;
	m_appliedWanted       = 0;
	m_sentWanted          = 0;
	m_sentWantedBorrowed  = false;
	// Instructions from a session that has ended. A reconnect to the same
	// session gets told again in the backfill, and a reconnect to a different
	// one would otherwise blow up cars on somebody else's say-so - by then
	// the generator may well have parked a fresh car in the space.
	for (PendingUnownedWreck &w : m_unownedWrecks)
		w = PendingUnownedWreck{};
	// Every door goes back to being this machine's own business, which is
	// single player behaving exactly as it always did. Pushed into the engine
	// seam rather than just cleared here, because a garage left held open
	// would stay held forever - the detour only stops holding when the union
	// says nobody wants it.
	for (uint32_t &mask : m_garageMasks)
		mask = 0;
	m_localGarageMask = 0;
	m_sentGarageMask  = 0;
	if (m_bridge.ApplyRemoteGarages)
		m_bridge.ApplyRemoteGarages(0);
	for (RemotePlayer &p : m_players) {
		if (p.poolHandle >= 0)
			UnseatPlayer(p);
		if (p.poolHandle >= 0 && m_bridge.DespawnRemote)
			m_bridge.DespawnRemote(p);
		p = RemotePlayer{};
	}
	// Same for vehicles: a car created for a session that's already ended
	// becomes a mission vehicle nothing will ever clean up, since both of
	// its deletion gates stay shut on purpose.
	// The car we are sitting in, if the session had one for it. The network
	// thread reconnects by itself, and the same session will hand that netId
	// back in the backfill: without this the roster builds a second car on
	// top of the one we are in and our claim then names the one we are in as
	// a new car. OnVehicleSpawn takes it up again instead.
	if (m_bridge.SampleLocalVehicleHandle) {
		const int32_t aboard = m_bridge.SampleLocalVehicleHandle();
		// At its wheel only. A passenger's car is its driver's to claim
		// back; adopted as ours, the let-go later built a copy of it on top
		// of the one we were still sitting in.
		for (const RemoteVehicle &v : m_vehicles)
			if (v.active && aboard >= 0 && v.poolHandle == aboard &&
			    (!m_bridge.LocalDrivesVehicle || m_bridge.LocalDrivesVehicle(v))) {
				m_rejoinNetId  = v.netId;
				m_rejoinModel  = v.modelId;
				m_rejoinHandle = aboard;
			}
	}
	// A passenger seat we told the old session about is news to the new one.
	m_localSeatNetId   = INVALID_NETID;
	m_pendingSeatNetId = INVALID_NETID;
	for (RemoteVehicle &v : m_vehicles) {
		// Not the car we claimed. That one is this engine's own traffic car,
		// very possibly with the local player sitting in it; the session
		// ending is not a reason to run a destructor over it. Same rule the
		// pedestrian half already keeps for the peds this machine hosts.
		if (v.poolHandle >= 0 && !v.ours && m_bridge.DespawnRemoteVehicle)
			m_bridge.DespawnRemoteVehicle(v);
		if (v.poolHandle >= 0 && v.ours && m_bridge.ReleaseOwnVehicle)
			m_bridge.ReleaseOwnVehicle(v);
		v = RemoteVehicle{};
	}
	// And every replica of somebody else's pedestrian, for the same reason.
	// The peds this machine *hosts* are untouched: those are the engine's own
	// pedestrians and they carry on being pedestrians after the session ends.
	ClearAmbientPeds();
	ClearAmbientCars();
	// Every replica of somebody else's helicopter. Our own go on chasing us
	// as they do in single player; the next session hears of them under new
	// serials.
	m_helis.Clear();
	// Every pickup goes back to being the local engine's own business. Not
	// an undo: the only lasting thing the seam does to the pickup table is
	// remove a pickup somebody else collected, which is what this machine's
	// own engine would have done anyway and which the game respawns by
	// itself. What is dropped here is the gate.
	if (m_bridge.PickupsReset)
		m_bridge.PickupsReset();
}

void Client::PreFrame() {
	const bool connected = m_net.IsConnected();
	if (m_wasConnected && !connected) {
		// Everything in the roster now refers to a session that's gone.
		Log("client: connection lost, clearing %u remote player(s)", RemoteCount());
		ForgetRejoin();
		ClearRoster();
		m_feed.Push(FeedKind::Notice, "lost the connection to the server",
		            WallClock::NowMs());
	}
	m_wasConnected = connected;

	m_scratch.clear();
	m_net.DrainInbound(m_scratch);
	for (const Message &msg : m_scratch)
		HandleMessage(msg);

	// After the drain, which is where the host can change hands.
	PushCheatSession();

	// Before CGame::Process, which is where CTrain::UpdateTrains and
	// CPlane::UpdatePlanes run.
	m_sessionTime.Tick(WallClock::NowMs());
	if (m_bridge.SetSessionClock)
		m_bridge.SetSessionClock(m_sessionTime.Valid(), m_sessionTime.OffsetMs());

	UpdateRemotes();
	UpdateRemoteVehicles();
	NoteVehicleHolders();
	// Cars nobody owns that the session says are finished. Before the frame's
	// simulation rather than after it, because this ends with the engine's own
	// BlowUpCar and the explosion it starts should be part of the frame like
	// any other.
	UpdateUnownedWrecks();
	// Replicas of other people's pedestrians. Before the seat pass for the
	// same reason the vehicle pass is: everything that creates an entity runs
	// before anything that relates two of them.
	UpdateRemoteAmbientPeds();
	UpdateRemoteAmbientCars();
	// After every pass above that builds or destroys a copy of a session car,
	// and before CGame::Process, which is where the traffic generator reads
	// the cap this writes.
	UpdateTrafficAllowance();
	// Other players' helicopters. Placed here and moved inside CWorld::Process
	// by the detour on CHeli::ProcessControl, which is where the owner's own
	// helicopter moves too.
	m_helis.Tick(WallClock::NowMs());
	// Runs after both spawn passes, so a ped and a car appearing on the same
	// frame get seated that same frame.
	UpdateRemoteSeats();
	// And the same for the city's own drivers. After the two ambient spawn
	// passes for the same reason, and before the pose pass below because a
	// ped that has just been seated must not then have its position written
	// over the top of the seat.
	UpdateAmbientPedSeats();
	// Every frame, like a remote player's pose and unlike a remote car's
	// transform. WorldBridge::ApplyAmbientPedState is where that difference
	// is argued.
	ApplyAmbientPedPoses();
	ReportAmbientFreshness();

	// Doors, before CGame::Process rather than after it. CGarages::Update is
	// called from inside CGame::Process, so the union has to be in place
	// before the detour runs - written afterwards it would be one frame late
	// every frame, which for a door that is being held open by exactly one
	// frame's worth of ramp is the difference between held and oscillating.
	UpdateGarages();

	// Last, and in PreFrame rather than PostFrame, because both halves of
	// that matter. Last, because the floor is a question about the roster and
	// the roster has just been brought up to date by the drain above. And
	// before CGame::Process, because the two police generators read this
	// number from inside it - CPopulation::AddToPopulation and
	// CCarCtrl::GenerateOneRandomCar - so a level written here takes effect
	// on this frame instead of the next one.
	TickWanted();
}

void Client::PostFrame() {
	// Nothing of ours goes out until the session has welcomed us: the server
	// takes it from the moment our hello lands, and a claim sent now is
	// answered after the welcome's ClearRoster has emptied the table that
	// would recognise the answer. What the player did meanwhile is dropped,
	// not held - a death or a blast from while we were away would be played
	// on everybody else's screen after the fact.
	if (!m_net.IsConnected() || m_localPlayerId == 0xFF) {
		DiscardLocalCombat();
		return;
	}
	// Wall clock, not frames - the target install runs at 60 FPS and 60/25
	// isn't an integer (docs/compat.md §2.4). This runs every frame, not at
	// the send rate: it happens after CGame::Process and before the frame
	// draws, and its job is undoing the frame of local physics that just
	// happened to a car somebody else is driving. Has to run on every frame
	// physics ran, not just the frames we happen to be sending on.
	CorrectRemoteVehicles();
	// Replicas of somebody else's traffic, for the same reason and in the
	// same place. A locked mission car with no driver still has gravity, a
	// suspension and a handbrake the engine is free to work on.
	CorrectAmbientCars();

	// Our own car's blast, first of all the events. The server takes it only
	// from the player it believes is driving that car, and BlowUpCar kills
	// the driver in the same frame: a death that went out first took us out
	// of the seat on the server, and the blast behind it was thrown away as
	// coming from nobody, so the car never blew up on anybody else's screen.
	// Still ahead of the exit, which goes on the send tick below.
	SendLocalVehicleBlasts();

	// Above the rate limiter on purpose. A shot is an event, and the limiter
	// is there to thin out a *stream* - holding a muzzle flash back for up
	// to 40ms would let a second shot overtake it on a reliable-ordered
	// channel, and a burst would arrive as one clump.
	SendLocalCombat();
	// And a line the player typed, on the same reliable channel.
	SendTypedChat();

	// Above the limiter too, and for the same reason: a pedestrian born and
	// reaped between two 25 Hz ticks would otherwise be announced after it had
	// already stopped existing. These are reliable events, not a stream.
	SendLocalAmbientPeds();
	SendLocalAmbientCars();

	// And the unowned cars our own engine just destroyed, above the limiter
	// with the rest of the reliable events.
	SendUnownedBlasts();

	// Hits on other people's cars, and they go here rather than down with the
	// blasts for two reasons. The server checks this one against who is
	// driving the *target*, not against who is driving ours, so nothing about
	// it has to be ordered against our own enter or exit. And it is the only
	// packet in this file that can fire several times in one frame - a burst
	// is one per round - so holding it for the 25 Hz tick would collapse the
	// queue into a batch and add up to 40 ms to every shot.
	SendLocalVehicleHits();
	// And the same for traffic replicas, to the car's host (§1.23).
	SendLocalCarHits();

	// And the spray shop visits our own engine completed. Above the limiter
	// with the other reliable events, and after SendUnownedBlasts for no
	// reason beyond keeping the event block in one place: nothing orders
	// these two against each other.
	SendLocalResprays();

	// The traffic stream. Above the 25 Hz limiter because it has a slower
	// limiter of its own inside it, not because it is an event.
	SendHostedCarStates();
	// And the pedestrian one, for the same reason and on a limiter of its
	// own. Peds after cars so that on a tick where both fire, the traffic -
	// which is faster-moving and more wrong when it is late - goes first.
	SendHostedPedStates();

	// The cheats typed here that have to reach somebody. Before the world
	// send, because a sky cheat on the host is what asks that send to go now.
	SendLocalCheats();

	// The awards our engine forwarded this frame, and under a shared wallet
	// whatever our cash did. After CGame::Process, which is where all of it
	// happens.
	m_money.Send(m_localPlayerId, WallClock::NowMs());

	// Also above the limiter, with a 1 Hz one of its own inside it. A game
	// minute is a real second, so anything faster is repetition.
	SendLocalWorld();

	// Our police helicopters, at HELI_STATE_HZ on a limiter of their own, and
	// the reliable parts - a helicopter gone, a hit on somebody else's - as
	// soon as they happen.
	m_helis.Send(m_localPlayerId, WallClock::NowMs());

	if (!m_sendRate.Ready(WallClock::NowMs()))
		return;
	SendLocalModel();
	SendLocalLook();
	SendLocalState();
	// After the snapshot, not before. The snapshot carries the held weapon's
	// count, and sending the stored slots first would have a receiver apply
	// a stale hand before the current one arrived.
	SendLocalAmmo();
	// Before SendLocalVehicle, and that is the point of it: the claim below
	// goes out when our own entry has *finished*, and this goes out when it
	// started. Both are reliable and ordered, so an observer is told we are
	// getting into the car before it is told we are in it - which is the
	// order the two things happened in.
	SendLocalEntering();
	SendLocalVehicle();
	// The cars nobody is sitting in that this machine has been asked to
	// settle. On the send tick with SendLocalVehicle because it is the same
	// packet on the same channel; after it because a car we have just got
	// back into is a car we are driving, and the claim is what says so.
	SendCustodyVehicles();
	// And the traffic car the local player has just taken the wheel of, which
	// the session still files as somebody else's. protocol.h, S_CarPromoted.
	ClaimDrivenAmbientCars();
	// After SendLocalVehicle, because that is what claims a car and fills in
	// m_localVehicleNetId - a dent reported before the session has a name for
	// the car has nothing to name it with.
	SendLocalVehicleDamage();
	TickPassengerSeat();
	// Last: every car's correction for this frame is in, so what the probe
	// reads is what the frame draws.
	SendDesyncProbe(WallClock::NowMs());
}

void Client::HandleMessage(const Message &msg) {
	if (const S_Welcome *p = msg.as<S_Welcome>()) {
		OnWelcome(*p);
	} else if (const S_PlayerJoin *p = msg.as<S_PlayerJoin>()) {
		OnJoin(*p);
	} else if (const S_PlayerLeave *p = msg.as<S_PlayerLeave>()) {
		OnLeave(*p);
	} else if (const S_PlayerModel *p = msg.as<S_PlayerModel>()) {
		OnPlayerModel(*p);
	} else if (const S_PlayerLook *p = msg.as<S_PlayerLook>()) {
		OnPlayerLook(*p);
	} else if (const S_PlayerAmmo *p = msg.as<S_PlayerAmmo>()) {
		OnPlayerAmmo(*p);
	} else if (const S_PlayerState *p = msg.as<S_PlayerState>()) {
		OnPlayerState(*p);
	} else if (const S_VehicleSpawn *p = msg.as<S_VehicleSpawn>()) {
		OnVehicleSpawn(*p);
	} else if (const S_VehicleDespawn *p = msg.as<S_VehicleDespawn>()) {
		OnVehicleDespawn(*p);
	} else if (const S_VehicleState *p = msg.as<S_VehicleState>()) {
		OnVehicleState(*p);
	} else if (const S_VehicleBlowUp *p = msg.as<S_VehicleBlowUp>()) {
		OnVehicleBlowUp(*p);
	} else if (const S_VehicleDamage *p = msg.as<S_VehicleDamage>()) {
		OnVehicleDamage(*p);
	} else if (const S_PlayerPings *p = msg.as<S_PlayerPings>()) {
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			m_pings[id] = p->rttMs[id];
	} else if (const S_DesyncReport *p = msg.as<S_DesyncReport>()) {
		OnDesyncReport(*p);
	} else if (const S_VehicleHit *p = msg.as<S_VehicleHit>()) {
		OnVehicleHit(*p);
	} else if (const S_CarHit *p = msg.as<S_CarHit>()) {
		OnCarHit(*p);
	} else if (const S_UnownedBlowUp *p = msg.as<S_UnownedBlowUp>()) {
		OnUnownedBlowUp(*p);
	} else if (const S_EnterVehicle *p = msg.as<S_EnterVehicle>()) {
		OnEnterVehicle(*p);
	} else if (const S_EnteringVehicle *p = msg.as<S_EnteringVehicle>()) {
		OnEnteringVehicle(*p);
	} else if (const S_JackingVehicle *p = msg.as<S_JackingVehicle>()) {
		OnJackingVehicle(*p);
	} else if (const S_ExitVehicle *p = msg.as<S_ExitVehicle>()) {
		OnExitVehicle(*p);
	} else if (const S_VehicleCustody *p = msg.as<S_VehicleCustody>()) {
		OnVehicleCustody(*p);
	} else if (const S_CarPromoted *p = msg.as<S_CarPromoted>()) {
		OnCarPromoted(*p);
	} else if (const S_Shot *p = msg.as<S_Shot>()) {
		OnShot(*p);
	} else if (const S_Explosion *p = msg.as<S_Explosion>()) {
		OnExplosion(*p);
	} else if (const S_Damage *p = msg.as<S_Damage>()) {
		OnDamage(*p);
	} else if (const S_Death *p = msg.as<S_Death>()) {
		OnDeath(*p);
	} else if (const S_Respawn *p = msg.as<S_Respawn>()) {
		OnRespawn(*p);
	} else if (const S_WorldState *p = msg.as<S_WorldState>()) {
		OnWorldState(*p);
	} else if (const S_PedSpawn *p = msg.as<S_PedSpawn>()) {
		OnPedSpawn(*p);
	} else if (const S_PedDespawn *p = msg.as<S_PedDespawn>()) {
		OnPedDespawn(*p);
	} else if (const S_PedBodyPart *p = msg.as<S_PedBodyPart>()) {
		OnPedBodyPart(*p);
	} else if (const S_PedDeath *p = msg.as<S_PedDeath>()) {
		OnPedDeath(*p);
	} else if (const S_PedDamage *p = msg.as<S_PedDamage>()) {
		OnPedDamage(*p);
	} else if (const S_NpcShot *p = msg.as<S_NpcShot>()) {
		OnNpcShot(*p);
	} else if (const S_NpcDamage *p = msg.as<S_NpcDamage>()) {
		OnNpcDamage(*p);
	} else if (const S_NpcVehicleHit *p = msg.as<S_NpcVehicleHit>()) {
		OnNpcVehicleHit(*p);
	} else if (const S_CarSpawn *p = msg.as<S_CarSpawn>()) {
		OnCarSpawn(*p);
	} else if (const S_CarDespawn *p = msg.as<S_CarDespawn>()) {
		OnCarDespawn(*p);
	} else if (const S_AmbientAdopt *p = msg.as<S_AmbientAdopt>()) {
		OnAmbientAdopt(*p);
	} else if (const S_CarStates *p = msg.as<S_CarStates>()) {
		OnCarStates(*p);
	} else if (const S_PedStates *p = msg.as<S_PedStates>()) {
		OnPedStates(*p);
	} else if (const S_PickupTaken *p = msg.as<S_PickupTaken>()) {
		OnPickupTaken(*p);
	} else if (const S_PickupGrant *p = msg.as<S_PickupGrant>()) {
		OnPickupGrant(*p);
	} else if (const S_PickupDenied *p = msg.as<S_PickupDenied>()) {
		OnPickupDenied(*p);
	} else if (const S_PickupDrop *p = msg.as<S_PickupDrop>()) {
		OnPickupDrop(*p);
	} else if (const S_GarageState *p = msg.as<S_GarageState>()) {
		OnGarageState(*p);
	} else if (const S_Respray *p = msg.as<S_Respray>()) {
		OnRespray(*p);
	} else if (const S_ObjectBroken *p = msg.as<S_ObjectBroken>()) {
		OnObjectBroken(*p);
	} else if (const S_RampageOpen *p = msg.as<S_RampageOpen>()) {
		OnRampageOpen(*p);
	} else if (const S_RampageKill *p = msg.as<S_RampageKill>()) {
		OnRampageKill(*p);
	} else if (const S_RampageCar *p = msg.as<S_RampageCar>()) {
		OnRampageCar(*p);
	} else if (const S_RampageEnd *p = msg.as<S_RampageEnd>()) {
		OnRampageEnd(*p);
	} else if (const S_RampageVote *p = msg.as<S_RampageVote>()) {
		OnRampageVote(*p);
	} else if (const S_RampageTeleport *p = msg.as<S_RampageTeleport>()) {
		OnRampageTeleport(*p);
	} else if (const S_ObjectSettled *p = msg.as<S_ObjectSettled>()) {
		OnObjectSettled(*p);
	} else if (const S_Cheat *p = msg.as<S_Cheat>()) {
		OnCheat(*p);
	} else if (const S_HeliState *p = msg.as<S_HeliState>()) {
		// Only from somebody the roster has. A state is unreliable and can
		// arrive after its owner's S_PlayerLeave, and a replica built for a
		// player who has gone would have nobody to end it but the timeout.
		if (p->ownerPlayerId < MAX_PLAYERS && m_players[p->ownerPlayerId].active)
			m_helis.OnState(*p, m_localPlayerId, WallClock::NowMs());
	} else if (const S_HeliGone *p = msg.as<S_HeliGone>()) {
		m_helis.OnGone(*p, m_localPlayerId);
	} else if (const S_HeliHit *p = msg.as<S_HeliHit>()) {
		m_helis.OnHit(*p, m_localPlayerId);
	} else if (const S_HeliShot *p = msg.as<S_HeliShot>()) {
		// Unreliable like the state, and gated the same way.
		if (p->ownerPlayerId < MAX_PLAYERS && m_players[p->ownerPlayerId].active)
			m_helis.OnShot(*p, m_localPlayerId, WallClock::NowMs());
	} else if (const S_Money *p = msg.as<S_Money>()) {
		m_money.OnMoney(*p, m_localPlayerId);
		// The $250 for somebody else's helicopter is an award like any other.
		m_helis.SetShootDownPaid(m_money.Rule() != MONEY_RULE_OFF);
	} else if (const S_MoneyAward *p = msg.as<S_MoneyAward>()) {
		m_money.OnAward(*p, m_localPlayerId);
	} else if (const S_Chat *p = msg.as<S_Chat>()) {
		OnChat(*p);
	}
	// Anything else isn't handled yet, and stays silently ignored rather
	// than logged: a per-packet log line at 25 Hz would drown out the one
	// message that actually matters.
}

void Client::OnWelcome(const S_Welcome &pkt) {
	if (pkt.reject != 0) {
		// On the HUD too: a player who pressed nothing and got no session wants
		// to know which of the three it was, and only one of them is theirs to
		// fix. The socket thread stops asking for the other two.
		const char *why = pkt.reject == REJECT_BAD_VERSION ? "the server runs another CoopIII version"
		                  : pkt.reject == REJECT_FULL      ? "the server is full; asking again in a while"
		                  : pkt.reject == REJECT_BAD_PASSWORD
		                      ? "the server wants a password (CoopIII.ini, password)"
		                      : "the server turned us away";
		Log("client: server rejected the connection (reason %u): %s", pkt.reject, why);
		m_feed.Push(FeedKind::Notice, why, WallClock::NowMs());
		return;
	}
	m_friendlyFire     = (pkt.flags & SESSION_FRIENDLY_FIRE) != 0;
	m_ammoSync         = (pkt.flags & SESSION_AMMO_SYNC) != 0;
	const uint8_t rule = WantedRuleFromFlags(pkt.flags);
	Log("client: joined as player %u (netId %u), %u slots, %u Hz, %02u:%02u, "
	    "friendly fire %s, ammo sync %s, wanted level %s",
	    pkt.playerId, pkt.netId, pkt.maxPlayers, pkt.snapshotHz, pkt.hour, pkt.minute,
	    m_friendlyFire ? "on" : "off", m_ammoSync ? "on" : "off",
	    rule == WANTED_RULE_SHARED ? "shared"
	        : rule == WANTED_RULE_OFF ? "off" : "per player");

	// A welcome starts a session, so anything left from a previous one is stale.
	ClearRoster();
	// Everything our engine hosts is news to this session, whatever the last
	// one called it. On the game thread, which is why it is here and not in
	// ClearRoster (Client::Start runs that one on the boot thread).
	if (m_bridge.RestartHostedNames)
		m_bridge.RestartHostedNames();
	m_localPlayerId = pkt.playerId;
	m_localNetId    = pkt.netId;
	if (m_rejoinNetId != INVALID_NETID)
		m_rejoinUntilMs = WallClock::NowMs() + REJOIN_WAIT_MS;

	// On the HUD too, and differently the second time: the network thread
	// reconnects by itself after a loss, and a player who saw "lost the
	// connection" wants to see that it came back.
	m_feed.Push(FeedKind::Notice,
	            m_welcomes == 0 ? "connected to the session" : "back in the session",
	            WallClock::NowMs());
	++m_welcomes;
	// After ClearRoster, not before. It puts the wanted rule back to the
	// default, which is what a client with no session wants and would quietly
	// undo the line above if this were assigned first - the whole feature
	// then runs as per-player however the server is configured, which is a
	// bug that shows up only in a session somebody deliberately set to
	// `shared` or `off`.
	m_wantedRule    = rule;
	m_rampageRule   = RampageRuleFromFlags(pkt.flags);
	// After ClearRoster for the same reason again, and pushed to the seam at
	// once rather than on the next PreFrame for the reason ClearRoster gives.
	m_cheatRule     = CheatRuleFromFlags(pkt.flags);
	Log("client: cheats are %s in this session",
	    m_cheatRule == CHEAT_RULE_OFF        ? "off"
	    : m_cheatRule == CHEAT_RULE_PERSONAL ? "personal only"
	                                         : "shared");
	// After ClearRoster for the same reason the line above is.
	m_frenzyId       = NO_FRENZY;
	m_frenzyReported = false;
	if (m_bridge.SetRampageRule)
		m_bridge.SetRampageRule(m_rampageRule);

	// The one part of friendly fire the server can't enforce by refusing to
	// relay: an explosion is replayed locally at a position its owner chose,
	// so this machine is the one that has to decline the blast. See
	// SessionFlags in protocol.h.
	if (m_bridge.SetFriendlyFire)
		m_bridge.SetFriendlyFire(m_friendlyFire);
	// Same shape, and for a related reason: two of the places ammunition is
	// decided are inside the engine, where no packet reaches. See
	// WorldBridge::SetAmmoSync.
	if (m_bridge.SetAmmoSync)
		m_bridge.SetAmmoSync(m_ammoSync);
	// Nothing has been announced on this connection yet, whatever a previous
	// one got as far as saying. Back to "unowned, empty", which is what the
	// new session already believes about us.
	for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w) {
		m_sentAmmo[w]        = AmmoSlotBody{};
		m_sentAmmo[w].weapon = w;
	}
	SetHost(pkt.hostPlayerId);
	PushCheatSession();

	// The time of day is worth having before the first world packet, which
	// is up to a second away. Skipped when we turn out to be the host, since
	// the hour above is a copy of our own clock at best and the server's
	// stand-in at worst.
	WorldStateBody world{};
	world.hour       = pkt.hour;
	world.minute     = pkt.minute;
	world.weather    = pkt.weather;
	world.weatherOld = pkt.weatherOld;
	ApplyWorldState(world);

	// After ClearRoster, which throws the last session's estimate away.
	NoteServerTime(pkt.hdr.sendTimeMs);
}

void Client::OnJoin(const S_PlayerJoin &pkt) {
	if (pkt.playerId >= MAX_PLAYERS)
		return;
	// The server echoes our own join back to us; we're not our own remote.
	if (pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	p           = RemotePlayer{};
	p.active    = true;
	p.playerId  = pkt.playerId;
	p.netId     = pkt.netId;
	p.modelId   = pkt.modelId;

	char nick[NICK_LEN];
	std::memcpy(nick, pkt.nick, sizeof(nick));
	nick[NICK_LEN - 1] = '\0';   // wire field isn't guaranteed to be terminated
	p.nick             = nick;

	// The condition half of the packet (protocol 9). This is the same
	// announcement whether it is somebody arriving now or somebody who has
	// been in the session for twenty minutes being replayed to us, and the
	// difference between those two is entirely in these fields.
	p.dead        = (pkt.flags & PJF_DEAD) != 0;
	p.deathAnimId = p.dead ? pkt.deathAnimId : ANIM_NONE;

	if (pkt.flags & PJF_POS_VALID) {
		// Enough to create the ped from, before a single snapshot has
		// arrived. `last` is what SpawnRemote reads to decide where the ped
		// is born and what health it is born on, so it has to be coherent -
		// a half-filled one would put a player in a T-pose on zero health.
		p.last            = PlayerStateBody{};
		p.last.pos        = pkt.pos;
		p.last.heading    = pkt.heading;
		p.last.health     = pkt.health;
		p.last.armour     = pkt.armour;
		p.last.weapon     = pkt.weapon;
		// No animation was reported and none should be guessed at. Zero is
		// ANIM_STD_WALK, which would have a stationary player walking on the
		// spot until their first snapshot; ANIM_NONE is the wire's way of
		// saying "nothing in this slot" and leaves the engine's own choice
		// alone (docs/protocol.md §1.8).
		p.last.animId     = ANIM_NONE;
		p.last.animId2    = ANIM_NONE;
		p.haveState       = true;
		p.heardAtMs       = WallClock::NowMs();
		p.seedPose.pos     = pkt.pos;
		p.seedPose.heading = pkt.heading;
		p.haveSeedPose     = true;
	}

	Log("client: %s joined as player %u (%.0f hp, weapon %u%s%s)", p.nick.c_str(),
	    p.playerId, pkt.health, pkt.weapon, p.dead ? ", dead" : "",
	    (pkt.flags & PJF_POS_VALID) ? "" : ", position unknown");

	// Only somebody arriving now. Everybody we are told about on our own way
	// in was here already.
	if (pkt.flags & PJF_ARRIVED) {
		char line[FEED_MESSAGE];
		FormatJoined(line, sizeof line, p.nick.c_str());
		m_feed.Push(FeedKind::Notice, line, WallClock::NowMs());
	}

	// Two-phase spawn (docs/protocol.md §1.6): ask now, create later.
	if (m_bridge.RequestModel) {
		m_bridge.RequestModel(p.modelId);
		p.spawnPending = true;
	}
}

void Client::OnLeave(const S_PlayerLeave &pkt) {
	if (pkt.playerId >= MAX_PLAYERS)
		return;
	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	Log("client: %s left (reason %u)", p.nick.c_str(), pkt.reason);
	{
		char line[FEED_MESSAGE];
		FormatLeft(line, sizeof line, p.nick.c_str(), pkt.reason);
		m_feed.Push(FeedKind::Notice, line, WallClock::NowMs());
	}
	// Their doors go with them. A player who quits while standing in their
	// safehouse would otherwise hold that garage open on every other machine
	// for the rest of the session - the mask is a level and nothing else
	// would ever contradict it.
	m_garageMasks[pkt.playerId] = 0;
	if (p.poolHandle >= 0) {
		UnseatPlayer(p);
		if (m_bridge.DespawnRemote)
			m_bridge.DespawnRemote(p);
	}
	p = RemotePlayer{};

	// Their helicopters have nobody left to fly them. Each one climbs away
	// here and is removed, rather than hovering where the stream stopped.
	m_helis.OnOwnerLeft(pkt.playerId, WallClock::NowMs());

	// And every ownership they held over a car goes with them. The driver's
	// seat was always dropped here through the roster; a custody is the same
	// kind of claim and has to end the same way, on the same packet, or a car
	// they were settling would be left with a custodian who is not in the
	// session - which every machine reads as "somebody else is simulating it",
	// so nobody rests it, nobody pins it and it drifts on local physics on
	// each screen separately.
	//
	// The server does this to its own record in Session::RemovePeer. Doing it
	// here as well is not belt and braces: S_PlayerLeave is the only packet
	// sent for a disconnect, so this is the client's only chance to hear it.
	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active)
			continue;
		if (v.driverPlayerId == pkt.playerId)
			v.driverPlayerId = 0xFF;
		// Their clock goes too. Whoever takes the slot next has a clock of
		// their own, and a buffer still on the leaver's would drop every
		// sample of theirs older than the leaver's last. The car is held at
		// its last snapshot until somebody reports it again.
		if (v.reporterPlayerId == pkt.playerId) {
			v.interp           = VehicleInterpBuffer{};
			v.reporterPlayerId = 0xFF;
		}
		if (v.custodianPlayerId == pkt.playerId) {
			v.custodianPlayerId = INVALID_PLAYER;
			v.restFrames        = 0;
			v.settleReported    = false;
		}
	}
}

// A remote player is now wearing something else.
//
// The ped gets rebuilt rather than re-skinned. CPed::SetModelIndex can
// change a model in place, but it rebuilds the clump underneath anyway,
// which drops the animation, the weapon model, and the frame array we've
// been driving - so everything downstream would need re-driving regardless.
// Going back through the spawn path reuses two routes that are already
// proven, at the cost of one frame where the player isn't drawn.
//
// The seat, if they were in one, comes back on its own. UpdateRemoteSeats
// sees a player with no ped, forgets the seating, and redoes it once the
// new ped exists. There's a test for exactly this shape.
void Client::OnPlayerModel(const S_PlayerModel &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active || p.modelId == pkt.modelId)
		return;

	Log("client: %s is now model %u (was %u)", p.nick.c_str(), pkt.modelId, p.modelId);
	p.modelId = pkt.modelId;
	RebuildRemoteBody(p);
}

// Their model 0 is loaded under another name - the prison clothes on or off.
// Same rebuild as a model change: the look is decided when the ped is built
// (PrepareRemoteLook), so a new look needs a new ped.
void Client::OnPlayerLook(const S_PlayerLook &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	char look[PLAYER_LOOK_LEN];
	std::memcpy(look, pkt.look, sizeof look);
	if (!CleanPlayerLook(look) || std::memcmp(look, p.look, sizeof look) == 0)
		return;

	Log("client: %s is wearing '%s' (was '%s')", p.nick.c_str(), look,
	    p.look[0] ? p.look : "unknown");
	std::memcpy(p.look, look, sizeof look);

	// Only Claude has looks. Somebody on another model keeps their ped, and
	// the look is there for when they come back to model 0.
	if (p.modelId == 0)
		RebuildRemoteBody(p);
}

void Client::RebuildRemoteBody(RemotePlayer &p) {
	if (p.poolHandle >= 0) {
		UnseatPlayer(p);
		if (m_bridge.DespawnRemote)
			m_bridge.DespawnRemote(p);
		p.poolHandle = -1;
	}

	// A new ped starts with none of this applied, same as the first spawn -
	// including the death, if they were dead when they changed model. The
	// corpse we just destroyed was the thing carrying it.
	p.appliedAnimId  = ANIM_NONE;
	p.appliedAnimId2 = ANIM_NONE;
	p.appliedWeapon  = 0xFFFF;
	p.deathApplied   = false;

	p.spawnPending = true;
	if (m_bridge.RequestModel)
		m_bridge.RequestModel(p.modelId);
}

void Client::OnPlayerState(const S_PlayerState &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;   // state before join - the join is reliable and will still arrive

	p.last      = pkt.body;
	p.haveState = true;
	p.heardAtMs = WallClock::NowMs();
	// The held weapon's count arrives here rather than on a C_PlayerAmmo, so
	// this is where the slot table learns about it. Keeps the table complete
	// whichever hand the number came in, which is what lets a replayed shot
	// ask it for any weapon (combat.cpp) instead of only the current one.
	if (m_ammoSync && pkt.body.weapon < INVENTORY_SLOTS) {
		p.ammoKnown[pkt.body.weapon] = true;
		p.ammoClip[pkt.body.weapon]  = pkt.body.ammoClip;
		p.ammoTotal[pkt.body.weapon] = pkt.body.ammoTotal;
	}
	// Their own timeline has started, so the session's starting guess is done
	// with. Dropped here rather than left as a fallback on purpose: falling
	// back to it during a later stall would yank the ped back to wherever
	// they were standing when we joined, which is a teleport and not a
	// recovery.
	p.haveSeedPose = false;
	p.interp.Push(pkt.hdr.sendTimeMs, pkt.body.pos, pkt.body.heading,
	              MoveSpeedToMps(pkt.body.moveSpeed));
}

void Client::UpdateRemotes() {
	// Read once for the whole pass so every remote's playback clock advances
	// by the same amount this frame. Per-player sampling would give the last
	// one in the loop a few microseconds more than the first - harmless, but
	// no reason to leave it that way.
	const uint32_t nowMs = WallClock::NowMs();

	for (RemotePlayer &p : m_players) {
		if (!p.active)
			continue;

		// One sample per frame, used for both the spawn and the update.
		//
		// A remote ped must never exist at a position we haven't received
		// yet. The join is reliable and arrives first; the first state
		// snapshot is unreliable and arrives later. A ped created in that
		// gap is born at the world origin, which in GTA III is water, and
		// the engine just drowns it. Measured this in the live game once:
		// born PED_IDLE with 100 health at frame 14659, PED_DIE with 0
		// health at 14667, PED_DEAD by 14727. Every position written after
		// that landed on a corpse - which is why the ped got scanned,
		// accepted by the renderer, and still drew nothing at all.
		Pose pose;
		bool havePose = p.interp.SampleDelayed(nowMs, pose);

		// Nothing from them yet, but the session told us where it last saw
		// them. That is a real position somebody actually stood at, which is
		// the whole difference between this and the origin - see
		// RemotePlayer::seedPose. Lets a player who is loading, in the
		// frontend or in a cutscene exist for a joiner instead of being
		// invisible for as long as they stay there.
		if (!havePose && p.haveSeedPose) {
			pose     = p.seedPose;
			havePose = true;
		}

		// Finish the two-phase spawn once the streamer has the model AND we
		// know where to put it.
		if (p.poolHandle < 0 && p.spawnPending && havePose && m_bridge.IsModelReady &&
		    m_bridge.SpawnRemote) {
			// For Claude the model alone isn't the answer: model 0 is only
			// the right body when their look is the one ours is loaded as.
			const bool ready = m_bridge.PrepareRemoteLook
			                       ? m_bridge.PrepareRemoteLook(p)
			                       : m_bridge.IsModelReady(p.modelId);
			if (ready && m_bridge.SpawnRemote(p)) {
				p.spawnPending = false;
				// A new ped hasn't been killed, whatever happened to the last
				// one. Without this a corpse the engine took back (or that a
				// look change rebuilt) comes back standing.
				p.deathApplied = false;
				Log("client: spawned %s (handle %d)", p.nick.c_str(), p.poolHandle);
				// A new ped starts with an empty inventory, and the packets
				// that said what this player carries are long gone - they
				// are reliable, they arrived once, and they routinely beat
				// the model stream by whole seconds. So the table is
				// replayed onto the ped rather than the packets being kept.
				if (m_ammoSync && m_bridge.ApplyRemoteAmmo) {
					for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w) {
						if (!p.ammoKnown[w])
							continue;
						AmmoSlotBody slot;
						slot.weapon = w;
						slot.flags  = AMMO_SLOT_OWNED;
						slot.clip   = p.ammoClip[w];
						slot.total  = p.ammoTotal[w];
						m_bridge.ApplyRemoteAmmo(p, slot);
					}
				}
			}
		}

		if (p.poolHandle < 0)
			continue;

		// A ped that exists and a player the session says is dead: kill it,
		// once, on whichever frame both of those first become true.
		//
		// Driven from here rather than from the packet because the packet is
		// routinely early. A death can arrive while the ped is still
		// streaming in, and a *join* can announce somebody who has been dead
		// since before we connected - in both cases OnDeath's "if there is a
		// ped, kill it" simply drops the death and leaves a corpse walking
		// around on zero health. Same argument as UpdateRemoteSeats: drive
		// the state, don't handle the event.
		if (p.dead && !p.deathApplied && m_bridge.KillRemotePed) {
			m_bridge.KillRemotePed(p, p.deathAnimId);
			p.deathApplied = true;
			Log("client: %s laid out dead (anim %u)", p.nick.c_str(), p.deathAnimId);
		}

		if (havePose && m_bridge.ApplyRemotePose)
			m_bridge.ApplyRemotePose(p, pose);
	}
}

void Client::SendLocalModel() {
	if (!m_bridge.SampleLocalPlayerModel)
		return;

	uint16_t model = 0;
	if (!m_bridge.SampleLocalPlayerModel(model))
		return;   // no player ped right now - menus, loading, a cutscene

	if (model == m_sentModelId)
		return;

	C_PlayerModel pkt;
	InitHeader(pkt, WallClock::NowMs());
	pkt.modelId = model;
	m_net.Send(pkt, CH_EVENT);
	m_sentModelId = model;
	Log("client: telling the session we are model %u", model);
}

// Model 0's name, on change and once per session. Not gated on being model
// 0 ourselves: the look is what model 0 is here, and a receiver only uses it
// while we're on model 0 anyway.
void Client::SendLocalLook() {
	if (!m_bridge.SampleLocalPlayerLook)
		return;

	char look[PLAYER_LOOK_LEN] = {};
	if (!m_bridge.SampleLocalPlayerLook(look) || !CleanPlayerLook(look))
		return;
	if (std::memcmp(look, m_sentLook, sizeof look) == 0)
		return;

	C_PlayerLook pkt;
	InitHeader(pkt, WallClock::NowMs());
	std::memcpy(pkt.look, look, sizeof pkt.look);
	m_net.Send(pkt, CH_EVENT);
	std::memcpy(m_sentLook, look, sizeof look);
	Log("client: telling the session we are wearing '%s'", look);
}

// A weapon slot somebody is carrying but not holding.
//
// Recorded whether or not there is a ped to write it onto yet, the same
// two-field shape the death news uses: the packet is reliable and can easily
// beat the model stream. UpdateRemotes does not have to replay it, because
// ApplyRemoteAmmo is called again for every known slot the moment a ped is
// spawned (game/ped.cpp).
void Client::OnPlayerAmmo(const S_PlayerAmmo &pkt) {
	if (!m_ammoSync)
		return;
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;
	if (pkt.slot.weapon >= INVENTORY_SLOTS)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	// `ammoKnown` means "they have this weapon and this is how much is behind
	// it". A slot they no longer own goes back to unknown, so the table never
	// hands a rebuilt ped, or a joiner's backfill, a weapon its owner has put
	// down.
	const bool owned             = (pkt.slot.flags & AMMO_SLOT_OWNED) != 0;
	p.ammoKnown[pkt.slot.weapon] = owned;
	p.ammoClip[pkt.slot.weapon]  = owned ? pkt.slot.clip : 0;
	p.ammoTotal[pkt.slot.weapon] = owned ? pkt.slot.total : 0;

	if (p.poolHandle >= 0 && m_bridge.ApplyRemoteAmmo)
		m_bridge.ApplyRemoteAmmo(p, pkt.slot);
}

// Twelve slots' worth of change detection, once a tick, and almost always
// nothing to send.
//
// The held slot is skipped rather than diffed: it is in the snapshot that
// just went out, and it is the only one that changes at the rate a trigger
// does. Its last-sent value is still recorded, so putting a gun away and
// taking it out again does not produce a packet restating a number the
// session already had.
void Client::SendLocalAmmo() {
	if (!m_ammoSync || !m_bridge.SampleLocalAmmo)
		return;

	AmmoSlotBody slots[INVENTORY_SLOTS]{};
	uint8_t      held = 0;
	if (!m_bridge.SampleLocalAmmo(slots, held))
		return;   // no player ped right now

	// A burst cap rather than a rate limit. Thirteen slots only ever change
	// at once through a cheat or a mission grant, and one tick of catching up
	// is 40 ms - but thirteen reliable packets in front of a shot is not
	// something worth doing in one frame for a one-off.
	constexpr uint8_t MAX_PER_TICK = 4;
	uint8_t           sent         = 0;

	for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w) {
		const AmmoSlotBody &slot = slots[w];
		const AmmoSlotBody &sentSlot = m_sentAmmo[w];
		const bool unchanged = sentSlot.flags == slot.flags &&
		                       sentSlot.clip == slot.clip &&
		                       sentSlot.total == slot.total;

		// The one in our hands is carried by the snapshot. Recorded so that
		// putting it away and taking it out again does not produce a packet
		// restating a number the session already had, never sent.
		if (w == held) {
			m_sentAmmo[w] = slot;
			continue;
		}
		if (unchanged)
			continue;
		if (sent >= MAX_PER_TICK)
			break;   // the rest go out next tick, still on change

		C_PlayerAmmo pkt;
		InitHeader(pkt, WallClock::NowMs());
		pkt.slot = slot;
		m_net.Send(pkt, CH_EVENT);
		++sent;

		m_sentAmmo[w] = slot;
	}
}

void Client::SendLocalState() {
	if (!m_bridge.SampleLocalPlayer)
		return;

	PlayerStateBody body{};
	if (!m_bridge.SampleLocalPlayer(body))
		return;   // no player right now - menus, loading, a cutscene

	// Off the same sample, before it goes anywhere. Reading the ped a second
	// time just to look at one float would mean walking its animation list
	// twice a tick for nothing.
	UpdateLocalLife(body);

	// The stars TickWanted settled on a moment ago, not whatever the ped
	// sample happened to carry. SampleLocalPlayer does not read the wanted
	// level at all - it reads a CPed, and the wanted level is not on one -
	// so this is the only place the bits are set.
	body.flags = FlagsWithWanted(body.flags, m_sentWanted, m_sentWantedBorrowed);

	C_PlayerState pkt;
	InitHeader(pkt, WallClock::NowMs());
	pkt.body = body;
	m_net.Send(pkt, CH_SNAPSHOT);
}

// ---- the wanted level ------------------------------------------------------
//
// docs/wanted.md is the design. The three things worth knowing here:
//
//   - A player's stars are their own machine's to decide. Every input to
//     them - the crime, whether a cop saw it, the bribe, the spray, the
//     death, the arrest - happens in that process, and there is exactly one
//     CWanted in a GTA III process anyway.
//   - The police need nothing. They are ambient entities and
//     game/population.cpp has been replicating them all along.
//   - So all of this is: work out the floor the session entitles us to, and
//     write it into our own engine if our own engine is below it.

uint16_t Client::LocalCarNetId() const {
	if (m_localVehicleNetId != INVALID_NETID)
		return m_localVehicleNetId;
	return m_localSeatNetId;
}

uint8_t Client::WantedFloor() const {
	const uint16_t ourCar = LocalCarNetId();
	uint8_t        floor  = 0;
	for (const RemotePlayer &p : m_players) {
		// haveState, not active: a player we have been told about but never
		// heard a snapshot from is reporting a zeroed flags byte, which reads
		// as an un-borrowed zero and is indistinguishable from a player who
		// really has no stars. Harmless for a maximum, but it would also be
		// counted as a car occupant, so the test is here rather than implied.
		if (!p.active || !p.haveState)
			continue;
		if (!game::WantedPeerRaisesFloor(m_wantedRule, ourCar, p.seatVehicleNetId,
		                                 (p.last.flags & PF_WANTED_BORROWED) != 0))
			continue;
		const uint8_t level = WantedFromFlags(p.last.flags);
		if (level > floor)
			floor = level;
	}
	return floor;
}

void Client::TickWanted() {
	if (!m_bridge.SampleLocalWantedLevel)
		return;

	uint8_t engine = 0;
	if (!m_bridge.SampleLocalWantedLevel(engine)) {
		// No player ped: menus, loading, the gap between a death and a
		// respawn. Left entirely alone rather than treated as zero, because
		// zero is what a player who has just been busted also reads, and
		// recording one as the other would hand this player's own stars back
		// to them on the next tick as a borrowed level from the session.
		return;
	}

	const game::WantedPlan plan =
	    game::PlanWanted(m_wantedRule, engine, m_appliedWanted, m_ownWanted, WantedFloor());

	if (plan.write && m_bridge.WriteLocalWantedLevel) {
		if (plan.target > engine && !m_saidWantedRaised) {
			m_saidWantedRaised = true;
			Log("wanted: the session raised our stars from %u to %u (rule %u) "
			    "- said once", engine, plan.target, m_wantedRule);
		} else if (plan.target < engine && !m_saidWantedLowered) {
			m_saidWantedLowered = true;
			Log("wanted: the session lowered our stars from %u to %u (rule %u) "
			    "- said once", engine, plan.target, m_wantedRule);
		}
		m_bridge.WriteLocalWantedLevel(plan.target);
	}

	m_ownWanted          = plan.own;
	m_appliedWanted      = plan.applied;
	m_sentWanted         = plan.target;
	m_sentWantedBorrowed = plan.borrowed;
}

// ---- combat ----------------------------------------------------------------
//
// Shots and explosions get handled the moment they arrive rather than being
// folded into the per-frame reconciliation seats and vehicles use, and
// that's the whole difference between them: a seating is a *state* the
// engine has to be driven toward until it takes, while a shot is something
// that happened at one instant. Retry a muzzle flash next frame and it lands
// in the wrong place; retry it forever and one bullet fires as a burst.

void Client::OnShot(const S_Shot &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	// No ped yet means no barrel to fire out of. Dropped, not queued - by
	// the time the model finishes streaming this shot is old news, and a
	// player who's still loading hasn't missed anything worth replaying late.
	if (!p.active || p.poolHandle < 0 || !m_bridge.ReplayRemoteShot)
		return;

	m_bridge.ReplayRemoteShot(p, pkt.body);
}

void Client::OnExplosion(const S_Explosion &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active || !m_bridge.PlayRemoteExplosion)
		return;

	// Not gated on the ped existing, unlike a shot. An explosion carries its
	// own position and doesn't need a shooter to come out of, and the one
	// case where the thrower's ped is most likely missing - they were far
	// enough away to be out of stream range when they threw it - is exactly
	// the case where the fire still needs to show up.
	m_bridge.PlayRemoteExplosion(p, pkt.body);
}

// ---- damage, death and respawn ---------------------------------------------
//
// The asymmetry here is the design, not an accident. Damage arrives for us
// and we apply it to ourselves; death and respawn arrive for somebody else
// and we apply them to their ped. There is no packet in this group that lets
// one machine change another player's health, and that's the point
// (docs/protocol.md §1.10).

void Client::OnDamage(const S_Damage &pkt) {
	// The server sends this to the victim alone, but a relay is a relay.
	// Applying somebody else's hit to ourselves because the netId didn't
	// match would be the one failure this whole design exists to prevent.
	//
	// Both refusals say themselves once. Between these two lines and the
	// pair in game/combat.cpp, the log answers the whole question in one
	// glance: did the shooter decide a hit, did it reach the victim, did the
	// victim apply it. A round was lost to a chain that did all of that
	// silently and produced nothing.
	if (m_localNetId == INVALID_NETID || pkt.body.victimNetId != m_localNetId) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: a damage packet arrived for net %u and we are net %u, so it "
			    "is not ours to apply", pkt.body.victimNetId, m_localNetId);
		}
		return;
	}
	if (!m_bridge.ApplyRemoteDamage) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: damage is arriving but this build has no bridge to apply "
			    "it with, so nothing will ever hurt");
		}
		return;
	}

	// Blame, where we can resolve it. A null attacker still hurts: their ped
	// may not have streamed in, and the shot happened either way.
	RemotePlayer *attacker = nullptr;
	if (pkt.attackerId < MAX_PLAYERS && pkt.attackerId != m_localPlayerId &&
	    m_players[pkt.attackerId].active) {
		attacker            = &m_players[pkt.attackerId];
		m_lastAttackerNetId = attacker->netId;
		m_lastAttackerMs    = WallClock::NowMs();
	}

	m_bridge.ApplyRemoteDamage(attacker, pkt.body);
}

// Somebody else died. Their ped becomes a corpse, in whatever pose their own
// engine picked.
//
// The seat comes off first, and that isn't tidiness. CPed::SetDie's
// PED_DRIVING arm calls FlagToDestroyWhenNextProcessed on anything that
// isn't the player ped, and every remote player is a CCivilianPed - so
// killing a seated remote ped hands it to the engine to delete, and we find
// out a frame later when the pool handle stops resolving.
void Client::OnDeath(const S_Death &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	Log("client: %s died (killer net %u)", p.nick.c_str(), pkt.killerNetId);

	UnseatPlayer(p);
	// And stop asking for the seat back. Without this UpdateRemoteSeats sees
	// a standing instruction it can still carry out and puts the corpse back
	// behind the wheel on the very next frame.
	p.seatVehicleNetId = INVALID_NETID;
	LeaveDriversSeats(pkt.playerId);

	// Recorded, not carried out. UpdateRemotes does the killing, on the first
	// frame there is a ped to kill - which is this one when their ped is
	// already up, and a later one when the death caught them mid-stream. The
	// old "if there is a ped, kill it" simply lost the death in that second
	// case and left a player walking around on zero health.
	p.dead         = true;
	p.deathAnimId  = pkt.animId;
	p.deathApplied = false;
}

// Out of the driver's seat of every car, the way the session took them out
// of it (Session::NotePlayerDied, NotePlayerRespawned). No S_ExitVehicle is
// sent for either, and a row still naming them at the wheel outranks the
// custody their machine was just given for the car: every observer went on
// treating a corpse as its driver until the respawn.
void Client::LeaveDriversSeats(uint8_t playerId) {
	for (RemoteVehicle &v : m_vehicles)
		if (v.active && v.driverPlayerId == playerId)
			v.driverPlayerId = 0xFF;
}

// And they're back. The corpse is destroyed and a fresh ped built the same
// way the first one was, because CPed::SetDie has no undo: it zeroes the
// health, clears the collision and hands the clump a death animation.
void Client::OnRespawn(const S_Respawn &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	Log("client: %s respawned", p.nick.c_str());

	UnseatPlayer(p);
	p.seatVehicleNetId = INVALID_NETID;
	LeaveDriversSeats(pkt.playerId);
	if (p.poolHandle >= 0 && m_bridge.DespawnRemote)
		m_bridge.DespawnRemote(p);
	p.poolHandle = -1;

	// Alive again, and the ped about to be built for them must not be killed
	// on the frame it appears. Clearing `deathApplied` too so a *second*
	// death later is not mistaken for one that has already been carried out.
	p.dead         = false;
	p.deathAnimId  = ANIM_NONE;
	p.deathApplied = false;

	// The old life's snapshots are half a city away from the new one. Keeping
	// them would have the rebuilt ped rendered at the place its owner died
	// for the length of the interpolation delay, then snapped to the
	// hospital - so the buffer starts empty and the ped appears once real
	// positions arrive, about a snapshot and a delay later.
	p.interp.Clear();
	// And the session's starting guess for them is older still: it is where
	// they were standing when *we* joined. With the buffer empty it would be
	// the only pose on offer, so the rebuilt ped would appear back there.
	p.haveSeedPose = false;

	p.last.pos     = pkt.body.pos;
	p.last.heading = pkt.body.heading;
	p.last.health  = 100.0f;
	p.last.armour  = 0.0f;
	p.haveState    = true;

	// A new ped starts with none of this applied, same as after a model
	// change. Leave them set and the rebuilt player stands unarmed in a
	// T-pose until they next change weapon.
	p.appliedAnimId  = ANIM_NONE;
	p.appliedAnimId2 = ANIM_NONE;
	p.appliedWeapon  = 0xFFFF;

	p.spawnPending = true;
	if (m_bridge.RequestModel)
		m_bridge.RequestModel(p.modelId);
}

void Client::AnnounceDeath(uint16_t animId) {
	if (m_deathAnnounced)
		return;
	m_deathAnnounced = true;
	// A death takes over from an arrest, so the respawn that follows is the
	// death's and goes out once.
	m_arrestNoted    = false;

	const uint16_t killer = KillCreditFor(m_lastAttackerNetId, m_lastAttackerMs,
	                                      WallClock::NowMs(), KILL_CREDIT_MS);

	C_Death out;
	InitHeader(out, WallClock::NowMs());
	out.killerNetId = killer;
	out.animId      = animId;
	m_net.Send(out, CH_EVENT);
	Log("client: we died (killer net %u, anim %u)", killer, animId);
}

void Client::UpdateLocalLife(const PlayerStateBody &body) {
	const LifeEvent event =
	    LocalLifeEventFor(body.health, body.pedState, m_deathAnnounced, m_arrestNoted);
	if (event == LifeEvent::NOTHING)
		return;

	if (event == LifeEvent::DIED) {
		// Usually already announced by the time we get here: the
		// CPed::SetDie detour fires inside the frame and SendLocalCombat
		// drains it before this runs, with the animation the engine chose.
		// This is the path for a build where that hook didn't install.
		AnnounceDeath(ANIM_NONE);
		return;
	}

	if (event == LifeEvent::BUSTED) {
		// Nothing to send yet. The arrest itself is already on every other
		// screen: the pedState on this snapshot says PED_ARRESTED, the ped is
		// frozen where the cop caught it and still on the pose stream, and a
		// drag out of a car is carried by the snapshots too (DragLatchFor).
		// The police station is the part that needs a packet, below.
		m_arrestNoted = true;
		Log("client: we were busted at %.1f %.1f %.1f; the police station will "
		    "go out as a respawn",
		    body.pos.x, body.pos.y, body.pos.z);
		return;
	}

	// Alive again. GTA III resurrects the same ped rather than making a new
	// one, so from here a respawn is just health coming back, or the ped
	// leaving PED_ARRESTED - the position that goes with it is wherever the
	// hospital or the police station put us.
	const bool fromArrest = m_arrestNoted;
	m_deathAnnounced    = false;
	m_arrestNoted       = false;
	m_lastAttackerNetId = INVALID_NETID;
	m_lastAttackerMs    = 0;

	C_Respawn out;
	InitHeader(out, WallClock::NowMs());
	out.body.pos     = body.pos;
	out.body.heading = body.heading;
	m_net.Send(out, CH_EVENT);
	Log("client: we respawned at %.1f %.1f %.1f%s", body.pos.x, body.pos.y, body.pos.z,
	    fromArrest ? " (the police station)" : "");
}

// Our own car blowing up, on its way out.
//
// Only ever our own: the detour that fills this queue checks the car against
// the one the local player is driving, and a car we are not driving is not
// ours to declare finished. docs/protocol.md §1.11.2 is the rule and why.
// Riding in somebody else's car.
//
// Two halves, and only the first one is CoopIII's doing. Getting in is a key
// the original game has no binding for, so it is polled and acted on here.
// Getting out is the game's own exit key on a ped the engine knows is in a
// car, so there is nothing to intercept - the seat simply goes empty and the
// session is told.
//
// Nothing about the car goes on the wire from a passenger. The driver owns
// its physics, which is the same rule that decides who may report a car
// anywhere else in this file.
// How close you have to be to ask for a seat. GTA III's own enter key reaches
// about this far, so a passenger seat asks for the same standing.
constexpr float SEAT_RANGE_M = 8.0f;

void Client::TickPassengerSeat() {
	// An animated entry already in flight, driven before anything else: until
	// it resolves the player is neither on foot nor riding, and the checks
	// below would read them as on foot and start a second one.
	if (m_pendingSeatNetId != INVALID_NETID && m_bridge.PollLocalSeatEntry) {
		const int32_t walking = m_bridge.PollLocalSeatEntry();
		if (walking == SEAT_LOCAL_WALKING)
			return;

		const uint16_t netId = m_pendingSeatNetId;
		const uint8_t  asked = m_pendingSeatIndex;
		m_pendingSeatNetId   = INVALID_NETID;

		// The session was told at the start, so there is nothing to send for
		// the ordinary case. Two things can still need saying.
		//
		// The seat came out different. The slot was chosen before the walk -
		// the engine's animated entry needs a door, so it cannot be read back
		// afterwards the way the warp's was - and a second or two is long
		// enough for another ped to take it. Correct the session rather than
		// leave it naming a seat we are not in: the next joiner is told where
		// everybody is sitting from exactly that record.
		if (walking >= 0) {
			if (static_cast<uint8_t>(walking) != asked)
				AnnounceLocalSeat(netId, static_cast<uint8_t>(walking));
			return;
		}

		// Or it never happened at all: the car went, or the entry ran out of
		// time and the warp behind it could not seat us either. Nothing is
		// sent here on purpose - m_localSeatNetId is set from the start
		// announcement and LocalIsPassenger() is false, so the block below
		// sends the exit on the next tick. That is the same retraction a
		// player who gets out gets, and there is no reason for a second one.
		return;
	}

	const bool riding = m_bridge.LocalIsPassenger && m_bridge.LocalIsPassenger();

	// Out. Noticed rather than requested, because the way out is the game's.
	if (m_localSeatNetId != INVALID_NETID && !riding) {
		C_ExitVehicle out;
		InitHeader(out, WallClock::NowMs());
		out.netId = m_localSeatNetId;
		m_net.Send(out, CH_EVENT);
		Log("client: got out of vehicle %u", m_localSeatNetId);
		m_localSeatNetId = INVALID_NETID;
	}

	if (!m_bridge.LocalWantsSeatToggle || !m_bridge.SeatLocalPlayerIn)
		return;
	if (!m_bridge.LocalWantsSeatToggle())
		return;
	// Riding already: the same key gets us out. Driving is not ours to undo -
	// that is the engine's key and the engine's business.
	if (riding) {
		if (m_bridge.UnseatLocalPlayer)
			m_bridge.UnseatLocalPlayer();
		return;   // the exit packet goes on the next tick, once the seat reads empty
	}
	if (m_localVehicleNetId != INVALID_NETID)
		return;

	PlayerStateBody me{};
	if (!m_bridge.SampleLocalPlayer || !m_bridge.SampleLocalPlayer(me))
		return;

	// The nearest of the session's own cars, and only the session's: a car
	// nobody has claimed is local traffic, and seating somebody in one would
	// put them in a vehicle the other machine has never heard of.
	const RemoteVehicle *best     = nullptr;
	float                bestDist = SEAT_RANGE_M;
	for (const RemoteVehicle &v : m_vehicles) {
		if (!v.active || !v.haveState || v.poolHandle < 0)
			continue;
		const float dx = v.last.pos.x - me.pos.x;
		const float dy = v.last.pos.y - me.pos.y;
		const float dz = v.last.pos.z - me.pos.z;
		const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (d < bestDist) {
			bestDist = d;
			best     = &v;
		}
	}

	if (!best) {
		if (!m_saidSeatRefused) {
			m_saidSeatRefused = true;
			Log("client: asked for a passenger seat with none of the session's "
			    "cars within %.0f m. Traffic does not count - a car nobody has "
			    "claimed only exists on this machine",
			    SEAT_RANGE_M);
		}
		return;
	}

	uint8_t       asked = 0;
	const int32_t seat  = m_bridge.SeatLocalPlayerIn(best->poolHandle, &asked);
	if (seat == SEAT_LOCAL_WALKING) {
		// The engine is walking us to the door, and the session is told now -
		// at the start of the entry, not at the end of it.
		//
		// Waiting for the end was the whole of the bug this replaced. The
		// other machines have an animated replica entry of their own
		// (BeginSeatRemotePed, which opens the door the seat belongs to), and
		// it is the only thing that ever moves that car's door over there: a
		// door is swung by the entering ped's own animation, frame by frame,
		// and nothing about an open door travels. Told a second late, that
		// entry starts against a ped the pose stream has already carried into
		// the car - so the driver watched a passenger appear beside them with
		// the door shut.
		//
		// Told at the start, both engines play the same entry over the same
		// second. What this machine cannot promise at this point is the seat,
		// so `asked` is what goes out and the poll above corrects it.
		//
		// And nothing goes out at all in a build with no PollLocalSeatEntry,
		// because then there is nothing that could ever finish the entry or
		// take the claim back. Every bridge that can start one registers it;
		// this is the same null check every other call here gets.
		if (!m_bridge.PollLocalSeatEntry)
			return;
		m_pendingSeatNetId = best->netId;
		m_pendingSeatIndex = asked;
		Log("client: walking to vehicle %u to ride in it, %.1f m away when we "
		    "asked",
		    best->netId, bestDist);
		AnnounceLocalSeat(best->netId, asked);
		return;
	}
	if (seat < 0)
		return;   // game/seat.cpp said why

	AnnounceLocalSeat(best->netId, static_cast<uint8_t>(seat));
}

// One place that tells the session we are sitting in somebody else's car, so
// the warp fallback and the animated entry cannot drift apart in what they
// send or when they send it.
//
// Three callers, one packet each, and that is deliberate. The warp path sends
// the seat it was given, because it has one on the frame the key was pressed.
// The animated path sends the seat it asked for as the walk begins, and sends
// again only if the engine finished by handing over a different one. An entry
// that never finishes sends nothing from here at all - the exit that
// TickPassengerSeat notices on the next tick is the retraction.
void Client::AnnounceLocalSeat(uint16_t netId, uint8_t seat) {
	C_EnterVehicle out;
	InitHeader(out, WallClock::NowMs());
	out.body       = EnterVehicleBody{};
	out.body.netId = netId;
	out.body.seat  = seat;
	out.body.jack  = 0;
	m_net.Send(out, CH_EVENT);

	m_localSeatNetId = netId;
	Log("client: riding in vehicle %u, seat %u", netId, seat);
}

void Client::SendLocalVehicleBlasts() {
	if (!m_bridge.DrainLocalVehicleBlasts)
		return;

	constexpr uint8_t MAX_PER_FRAME = 4;
	LocalVehicleBlast blasts[MAX_PER_FRAME];
	const uint8_t     n = m_bridge.DrainLocalVehicleBlasts(blasts, MAX_PER_FRAME);
	if (n == 0)
		return;

	// The netId is read here rather than in the detour because this is where
	// it lives. If the session never gave our car a name - nobody else was
	// connected when we got in, or the claim is still in flight - there is
	// nothing to announce and the blast is purely local. Said out loud,
	// because "the car exploded and nobody else saw it" is exactly the
	// symptom this whole change is about and it must not have two silent
	// causes.
	if (m_localVehicleNetId == INVALID_NETID) {
		Log("client: our car blew up before the session had a name for it, so "
		    "nobody else will see it");
		return;
	}

	for (uint8_t i = 0; i < n; ++i) {
		C_VehicleBlowUp out;
		InitHeader(out, WallClock::NowMs());
		out.body.netId = m_localVehicleNetId;
		out.body.pos   = blasts[i].pos;
		out.body.rot   = blasts[i].rot;
		m_net.Send(out, CH_EVENT);
	}

	// m_localVehicleNetId is deliberately left alone. SendLocalVehicle runs
	// straight after this, sees the player is no longer in a car, and sends
	// the ordinary C_ExitVehicle - which is what tells the session nobody is
	// driving the wreck. Clearing it here would swallow that.
}

// ---- cars nobody owns (docs/roadmap.md 5.8) --------------------------------
//
// C_VehicleBlowUp is sent by the driver and only the driver, so a parked car
// has nobody to report it: nobody is in it, nobody says anything, and every
// joiner from then on is handed a pristine car standing where a burnt shell
// is on every other screen.
//
// The three functions below are the whole of the fix and they are small,
// which is itself a finding. Most of an unowned car's destruction already
// travelled: an explosion is replayed on every machine at an agreed position
// and CWorld::TriggerExplosion damages every car in the radius with a
// multiplier that depends on distance and nothing else, so a parked car blown
// up by a rocket is already a wreck everywhere (game/addresses.h, "an
// explosion damages every car in its radius"). This is the backstop for the
// cases that do not converge - accumulated gunfire, a shove from a replica,
// the five-second fire timer running from two different healths.

void Client::SendUnownedBlasts() {
	if (!m_bridge.DrainUnownedBlasts && !m_bridge.DrainAmbientWrecks)
		return;

	// Two queues, one send. game/vehicle.cpp fills the first off its BlowUpCar
	// detour (parked cars, and session cars nobody is driving);
	// game/population.cpp fills the second off its own sweep over the traffic
	// it hosts. Drained independently for the reason the apply loop is:
	// gating one on the other would have a half-installed bridge quietly drop
	// an arm of this.
	constexpr uint8_t MAX_PER_FRAME = 4;
	UnownedBlast      keys[MAX_PER_FRAME * 2];
	uint8_t           n = 0;
	if (m_bridge.DrainUnownedBlasts)
		n = m_bridge.DrainUnownedBlasts(keys, MAX_PER_FRAME);
	if (m_bridge.DrainAmbientWrecks)
		n = static_cast<uint8_t>(
		    n + m_bridge.DrainAmbientWrecks(keys + n, MAX_PER_FRAME));

	for (uint8_t i = 0; i < n; ++i) {
		C_UnownedBlowUp out;
		InitHeader(out, WallClock::NowMs());
		out.key   = keys[i].key;
		out.where = keys[i].where;
		m_net.Send(out, CH_EVENT);

		// Our own roster has to agree with what we just told everybody else.
		// The server relays this to everyone *except* us, so nothing comes
		// back to mark it - and an entry left un-destroyed is one the roster
		// would happily respawn as a new car the moment the engine reaps the
		// wreck. Only the session kind has a roster entry; a parked car
		// never had one, and an ambient one belongs to the machine sending
		// this - a host holds the car itself, not a replica of it.
		if (keys[i].key.kind == UNOWNED_SESSION)
			if (RemoteVehicle *v = VehicleSlot(keys[i].key.id, false))
				v->destroyed = true;

		// Once, the first time it works. This project has twice been bitten
		// by a feature whose success looked exactly like a feature nobody
		// installed, and "a parked car blew up and nobody else saw it" is
		// precisely the symptom this change exists to end.
		if (!m_saidUnownedWreckSent) {
			m_saidUnownedWreckSent = true;
			Log("client: told the session an unowned car (kind %u, id %u) blew "
			    "up here",
			    static_cast<unsigned>(keys[i].key.kind),
			    static_cast<unsigned>(keys[i].key.id));
		}
	}
}

// Hits our player landed on cars other people are driving or settling, and on
// session cars nobody holds, which the server gives to us to settle.
//
// The engine seam already knows which car each one is about - it found the
// Observed row for it in order to refuse the hit in the first place - so unlike
// the blast queue there is nothing here for Client to name. What is left is the
// send, and one check that is worth keeping on this side: the roster's own
// opinion of who drives that car.
//
// That check is deliberately not the whole of the arbitration. The server
// re-asks it (Session::VehicleHitRecipient) and is the one that decides, for
// the reason version 22 exists: a jack happens inside one process, so for a
// moment two machines each believe they drive a car and only the server can
// break the tie. This end drops what it already knows is pointless - a car we
// have since claimed ourselves, a wreck - rather than paying for the round
// trip.
void Client::SendLocalVehicleHits() {
	if (!m_bridge.DrainLocalVehicleHits)
		return;

	// Eight a frame. At 60 FPS that is 480 a second, far above any weapon's
	// rate of fire, so the cap is a bound on a runaway rather than a throttle
	// on play - and the queue behind it is sixteen deep, so a frame that
	// somehow produced more does not lose the newest.
	constexpr uint8_t MAX_PER_FRAME = 8;
	VehicleHitBody    hits[MAX_PER_FRAME];
	const uint8_t     n = m_bridge.DrainLocalVehicleHits(hits, MAX_PER_FRAME);

	for (uint8_t i = 0; i < n; ++i) {
		const RemoteVehicle *v = VehicleSlot(hits[i].netId, /*createIfMissing=*/false);
		if (!v || !VehicleHitIsWorthSending(*v, m_localVehicleNetId))
			continue;

		C_VehicleHit out;
		InitHeader(out, WallClock::NowMs());
		out.body = hits[i];
		m_net.Send(out, CH_EVENT);

		// A car nobody holds is said by the detour instead: the server is about
		// to make us its custodian, so there is no "their machine" to name.
		const bool held = v->driverPlayerId != INVALID_PLAYER ||
		                  v->custodianPlayerId != INVALID_PLAYER;
		if (held && !m_saidVehicleHitSent) {
			m_saidVehicleHitSent = true;
			const bool driven = v->driverPlayerId != INVALID_PLAYER;
			Log("client: told the session we shot car %u, which player %u is %s, "
			    "for %.0f with cause %u; their machine decides what it costs",
			    hits[i].netId,
			    static_cast<unsigned>(driven ? v->driverPlayerId
			                                 : v->custodianPlayerId),
			    driven ? "driving" : "settling", hits[i].amount,
			    static_cast<unsigned>(hits[i].weapon));
		}
	}

	// And our pedestrians' rounds on those cars. Only a car somebody holds is
	// ever queued (game/vehicle.h, DecideCarDamage), and the server refuses
	// the rest.
	if (!m_bridge.DrainNpcVehicleHits)
		return;
	NpcVehicleHit npcHits[MAX_PER_FRAME];
	const uint8_t m = m_bridge.DrainNpcVehicleHits(npcHits, MAX_PER_FRAME);
	for (uint8_t i = 0; i < m; ++i) {
		const RemoteVehicle *v = VehicleSlot(npcHits[i].body.netId, /*createIfMissing=*/false);
		if (!v || !VehicleHitIsWorthSending(*v, m_localVehicleNetId))
			continue;
		C_NpcVehicleHit out;
		InitHeader(out, WallClock::NowMs());
		out.attackerPedNetId = npcHits[i].pedNetId;
		out.body             = npcHits[i].body;
		m_net.Send(out, CH_EVENT);
	}
}

// Somebody shot the car we are driving, or the one we are settling - which
// includes a parked car we shot ourselves: the server made us its custodian
// and sent our own hit back, right behind the S_VehicleCustody saying so.
//
// There is no roster row to write and nothing to remember, which is what makes
// this the shortest of the vehicle handlers. A hit is not a fact about the car
// - it is a thing that happened to whoever owned it when the trigger was
// pulled - so it is applied or it is dropped, and the health it produced goes
// back out on the snapshot that has always carried it.
//
// Dropped, rather than applied late, when whatever made it ours has ended by
// the time it lands: we got out, the settle finished, somebody else got in.
// The server only routes a hit to the driver or, with no driver, the
// custodian, so the car was ours when it left the server, and every one of
// those endings is newer than the hit.
void Client::OnVehicleHit(const S_VehicleHit &pkt) {
	if (!m_bridge.ApplyRemoteVehicleHit) {
		// Said once, and it is the line that tells "this build cannot" apart
		// from "this build would not". A whole session was lost to a chain
		// that did nothing and said nothing about which link stopped it.
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: somebody shot our car and this build has no bridge to "
			    "apply it with, so nobody will ever be able to damage our car");
		}
		return;
	}

	bool           driving = false;
	RemoteVehicle *v       = CarForReportedHit(pkt.body.netId, driving);
	if (!v)
		return;

	// Blame, where it resolves. A null attacker still damages the car: the
	// shooter's ped may not have streamed in here, and the shot happened
	// either way. Deliberately not folded into m_lastAttackerNetId - that
	// field is who last hurt the local *player*, and a dent in the bodywork
	// has no bearing on who gets credit for killing us.
	RemotePlayer *attacker = nullptr;
	if (pkt.attackerId < MAX_PLAYERS && pkt.attackerId != m_localPlayerId &&
	    m_players[pkt.attackerId].active)
		attacker = &m_players[pkt.attackerId];

	m_bridge.ApplyRemoteVehicleHit(*v, attacker, pkt.body, /*settling=*/!driving);

	// Kept a moment longer than the car takes to stop, so the rest of a burst
	// lands in the same custody (CUSTODY_HIT_HOLD_MS).
	if (!driving)
		v->holdUntilMs = WallClock::NowMs() + CUSTODY_HIT_HOLD_MS;
}

RemoteVehicle *Client::CarForReportedHit(uint16_t netId, bool &driving) {
	RemoteVehicle *v = VehicleSlot(netId, /*createIfMissing=*/false);
	if (!v || !v->active)
		return nullptr;

	// Our car, as the roster has it. The engine seam asks the same question of
	// CVehicle::m_pDriver before it calls anything, and both asks are wanted:
	// this one stops a packet for somebody else's car reaching the seam at all,
	// and that one is the answer that counts, because a jack moves the engine's
	// idea of the driver before the session's catches up.
	//
	// A car the session has already written off is refused here too. Nothing
	// arrives after that which is worth applying, and BlowUpRemoteVehicle has
	// by now either run or been told to.
	driving = DrivenLocally(*v);
	if (!TakeReportedVehicleHit(*v, driving, m_localPlayerId)) {
		if (!driving && HaveCustodyOf(*v) && !m_saidLateCustodyHit) {
			m_saidLateCustodyHit = true;
			Log("client: a hit on vehicle %u arrived after we told the session "
			    "we were finished settling it. Dropped - we have stopped "
			    "reporting that car", v->netId);
		}
		return nullptr;
	}
	return v;
}

// One of somebody else's pedestrians shot the car we drive or settle. The
// same car and the same custody hold as a player's hit on it.
void Client::OnNpcVehicleHit(const S_NpcVehicleHit &pkt) {
	if (!m_bridge.ApplyNpcVehicleHit)
		return;
	bool           driving = false;
	RemoteVehicle *v       = CarForReportedHit(pkt.body.netId, driving);
	if (!v)
		return;

	RemoteAmbientPed *attacker = AmbientPedByNetId(pkt.attackerPedNetId);
	if (attacker && attacker->ownerPlayerId != pkt.ownerPlayerId)
		attacker = nullptr;

	static bool said = false;
	if (!said) {
		said = true;
		Log("client: somebody else's pedestrian shot our car - net %u, hosted by player "
		    "%u, car %u, %.0f with cause %u", pkt.attackerPedNetId, pkt.ownerPlayerId,
		    pkt.body.netId, pkt.body.amount, pkt.body.weapon);
	}
	m_bridge.ApplyNpcVehicleHit(*v, attacker, pkt.body, /*settling=*/!driving);
	if (!driving)
		v->holdUntilMs = WallClock::NowMs() + CUSTODY_HIT_HOLD_MS;
}

// Hits our player landed on replicas of other machines' traffic, on their way
// to each car's host (§1.23). The detour already refused them locally, so a
// hit dropped here is a hit nobody applies - which is right for every case
// CarHitIsWorthSending turns away and wrong for none of them.
void Client::SendLocalCarHits() {
	if (!m_bridge.DrainLocalCarHits)
		return;

	constexpr uint8_t MAX_PER_FRAME = 8;   // same bound as SendLocalVehicleHits
	VehicleHitBody    hits[MAX_PER_FRAME];
	const uint8_t     n = m_bridge.DrainLocalCarHits(hits, MAX_PER_FRAME);

	for (uint8_t i = 0; i < n; ++i) {
		const RemoteAmbientCar *car = AmbientCarByNetId(hits[i].netId);
		if (!car || !CarHitIsWorthSending(*car, m_localPlayerId))
			continue;

		C_CarHit out;
		InitHeader(out, WallClock::NowMs());
		out.body = hits[i];
		m_net.Send(out, CH_EVENT);

		if (!m_saidCarHitSent) {
			m_saidCarHitSent = true;
			Log("client: told the session we shot player %u's traffic car %u for "
			    "%.0f with cause %u; their machine decides what it costs",
			    static_cast<unsigned>(car->ownerPlayerId), hits[i].netId,
			    hits[i].amount, static_cast<unsigned>(hits[i].weapon));
		}
	}
}

// Somebody shot a replica of traffic we host. There's no roster row for our
// own traffic on this side - population.cpp holds those - so this only
// resolves who fired and hands the netId down.
void Client::OnCarHit(const S_CarHit &pkt) {
	if (!m_bridge.ApplyHostedCarHit) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: somebody shot our traffic and this build has no bridge "
			    "to apply it with, so our traffic can't be hurt from another "
			    "machine");
		}
		return;
	}

	RemotePlayer *attacker = nullptr;
	if (pkt.attackerId < MAX_PLAYERS && pkt.attackerId != m_localPlayerId &&
	    m_players[pkt.attackerId].active)
		attacker = &m_players[pkt.attackerId];

	m_bridge.ApplyHostedCarHit(pkt.body.netId, attacker, pkt.body);
}

void Client::OnUnownedBlowUp(const S_UnownedBlowUp &pkt) {
	// Nothing is applied here. The car may be three streets away and not
	// streamed in - which is the *normal* case for a backfill, where a joiner
	// is handed up to a minute of them at once. So this records a standing
	// instruction and UpdateUnownedWrecks carries it out, which is the shape
	// UpdateRemoteSeats settled on for the same class of problem: the packet
	// and the thing it is about turn up in either order.
	for (PendingUnownedWreck &w : m_unownedWrecks)
		if (w.active && w.key.kind == pkt.key.kind && w.key.id == pkt.key.id)
			return;   // already holding this one

	for (PendingUnownedWreck &w : m_unownedWrecks)
		if (!w.active) {
			w.active   = true;
			w.key      = pkt.key;
			w.where    = pkt.where;
			w.byPlayer = pkt.reporterPlayerId;
			w.sinceMs  = WallClock::NowMs();
			return;
		}

	// Full. Said once rather than never: a dropped instruction is a car that
	// stays intact here and is a wreck everywhere else, which is the bug.
	if (!m_saidUnownedWrecksFull) {
		m_saidUnownedWrecksFull = true;
		Log("client: holding %zu unapplied unowned wrecks already; dropping "
		    "(kind %u, id %u) and this will not be said again)",
		    MAX_PENDING_UNOWNED_WRECKS, static_cast<unsigned>(pkt.key.kind),
		    static_cast<unsigned>(pkt.key.id));
	}
}

// A car the session knows about that nobody is driving.
//
// docs/roadmap.md 5.8's literal case: somebody claimed a car, drove it,
// parked it and walked away, and then it was blown up. C_VehicleBlowUp is
// accepted from the driver and there is no driver, so nothing about it ever
// reached the wire and the session kept a healthy row for it forever.
//
// Not handed to the bridge's WreckUnownedVehicle, because that one resolves a
// key the *engine* understands and a netId is the session's name rather than
// the game's. The roster is what translates one into the other, and it lives
// here.
UnownedWreckOutcome Client::WreckSessionVehicle(uint16_t netId) {
	RemoteVehicle *v = VehicleSlot(netId, /*createIfMissing=*/false);
	if (!v)
		// A car we have never been told about, or one already forgotten.
		// Nothing to build from - the packet carries no model - and a blast
		// is not a reason to invent a car.
		return UnownedWreckOutcome::BadKey;

	if (v->destroyed)
		return UnownedWreckOutcome::Already;

	// Marked before the replay and whatever the replay does, exactly as
	// OnVehicleBlowUp marks it: a car the session says is wrecked must never
	// be respawned as a new one, and "the pool does not have it right now" is
	// not a reason to keep believing it is fine.
	v->destroyed = true;
	v->interp    = VehicleInterpBuffer{};

	for (RemotePlayer &p : m_players)
		if (p.active && p.InvolvedWith(netId))
			UnseatPlayer(p);
	for (RemotePlayer &p : m_players)
		if (p.active && p.seatVehicleNetId == netId)
			p.seatVehicleNetId = INVALID_NETID;
	v->driverPlayerId = 0xFF;

	if (v->poolHandle < 0 || !m_bridge.BlowUpRemoteVehicle)
		return UnownedWreckOutcome::BadKey;

	// At the transform the session last heard, which is not a compromise: the
	// car is parked, so the last thing its driver said about it before
	// getting out is exactly where it still is. The packet carries no
	// transform for precisely this reason - every machine already has the
	// same one.
	m_bridge.BlowUpRemoteVehicle(*v, v->last.pos, v->last.rot);
	return UnownedWreckOutcome::Wrecked;
}

// A traffic car somebody else's engine made, and somebody else's engine
// destroyed.
//
// docs/roadmap.md 5.8's ambient half. The netId is the session's name for a
// car on the ambient roster, so the roster is what translates it - the same
// division WreckSessionVehicle draws, and the reason neither of them goes
// through the bridge's key resolver, which speaks names the *map* hands out.
//
// Only the machine hosting a car ever sends one of these (the server refuses
// anybody else's), so there is no arbitration here at all: this is somebody
// telling us what their own engine did to their own car.
UnownedWreckOutcome Client::WreckAmbientCar(uint16_t netId, const BlastTransform &where) {
	RemoteAmbientCar *car = AmbientCarByNetId(netId);
	if (!car)
		// Never heard of it, or already despawned. Nothing to build from -
		// the packet carries no model - and a wreck is not a reason to invent
		// a car.
		return UnownedWreckOutcome::BadKey;

	if (!m_bridge.WreckAmbientCarReplica)
		return UnownedWreckOutcome::BadKey;

	// Marked before the replay and whatever the replay does, exactly as
	// OnVehicleBlowUp and WreckSessionVehicle mark theirs: "the pool does not
	// have it right now" is not a reason to go on believing the car is fine,
	// and UpdateRemoteAmbientCars would otherwise build a pristine replica of
	// it the moment the local engine reaped the shell.
	car->destroyed = true;

	// Already a wreck here is the ordinary answer rather than a failure: the
	// explosion that did it was replayed on this machine too and our own
	// engine got there first.
	return m_bridge.WreckAmbientCarReplica(*car, where);
}

void Client::UpdateUnownedWrecks() {
	// Any one seam is enough to be worth running: the session kind goes
	// through BlowUpRemoteVehicle, the parked kind through
	// WreckUnownedVehicle, the ambient kind through WreckAmbientCarReplica,
	// and each arm checks its own. Gating the whole loop on one of them would
	// have a half-installed bridge silently drop the others - which is the
	// failure mode this project keeps being bitten by, a feature whose
	// success looks exactly like one nobody installed.
	if (!m_bridge.WreckUnownedVehicle && !m_bridge.BlowUpRemoteVehicle &&
	    !m_bridge.WreckAmbientCarReplica)
		return;

	const uint32_t now = WallClock::NowMs();

	for (PendingUnownedWreck &w : m_unownedWrecks) {
		if (!w.active)
			continue;

		UnownedWreckOutcome outcome = UnownedWreckOutcome::BadKey;
		switch (w.key.kind) {
		case UNOWNED_SESSION:
			outcome = WreckSessionVehicle(w.key.id);
			break;
		case UNOWNED_AMBIENT:
			outcome = WreckAmbientCar(w.key.id, w.where);
			break;
		default:
			if (m_bridge.WreckUnownedVehicle)
				outcome = m_bridge.WreckUnownedVehicle(w.key);
			break;
		}

		switch (outcome) {
		case UnownedWreckOutcome::Wrecked:
			if (!m_saidUnownedWreckApplied) {
				m_saidUnownedWreckApplied = true;
				Log("client: blew up an unowned car (kind %u, id %u) because "
				    "player %u did",
				    static_cast<unsigned>(w.key.kind),
				    static_cast<unsigned>(w.key.id),
				    static_cast<unsigned>(w.byPlayer));
			}
			w = PendingUnownedWreck{};
			continue;

		case UnownedWreckOutcome::Already:
			// The ordinary outcome, and the one that says the explosion
			// replay is doing its job. Silent on purpose.
		case UnownedWreckOutcome::BadKey:
			w = PendingUnownedWreck{};
			continue;

		case UnownedWreckOutcome::NotHere:
			break;
		}

		// Not here yet. Keep asking, but only for as long as the server would
		// have kept the record - past that the engine has had time to clear
		// the shell and let the generator park a fresh car in the space, and
		// a retry would start destroying cars that were never destroyed.
		if (now - w.sinceMs >= UNOWNED_WRECK_RETRY_MS)
			w = PendingUnownedWreck{};
	}
}

void Client::DiscardLocalCombat() {
	if (!m_bridge.DrainLocalCombat)
		return;
	CombatEvent stale[16];
	while (m_bridge.DrainLocalCombat(stale, 16) != 0) {
	}
}

void Client::SendLocalCombat() {
	if (!m_bridge.DrainLocalCombat)
		return;

	// Bounded per frame instead of "everything pending". The drain is the
	// only thing that empties the bridge's queue, so an unbounded loop here
	// would mean unbounded work on the game thread if anything upstream ever
	// went wrong. The queue itself drops its oldest entries instead of
	// growing without limit.
	constexpr uint8_t MAX_PER_FRAME = 8;
	CombatEvent       events[MAX_PER_FRAME];
	const uint8_t     n = m_bridge.DrainLocalCombat(events, MAX_PER_FRAME);

	Vec3     viewers[MAX_PLAYERS];
	uint32_t viewerCount = 0;
	bool     placed      = false;

	for (uint8_t i = 0; i < n; ++i) {
		switch (events[i].kind) {
		case CombatEvent::SHOT: {
			C_Shot out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].shot;
			m_net.Send(out, CH_EVENT);
			break;
		}
		case CombatEvent::EXPLOSION: {
			C_Explosion out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].explosion;
			m_net.Send(out, CH_EVENT);
			break;
		}
		case CombatEvent::DAMAGE: {
			// A hit our engine resolved on somebody else's ped and was
			// stopped from applying. The server decides whether it's
			// allowed to land at all (friendly fire), and the victim
			// decides what it does to them.
			C_Damage out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].damage;
			m_net.Send(out, CH_EVENT);
			break;
		}
		case CombatEvent::PED_DAMAGE: {
			// The same thing about a pedestrian somebody else hosts. The
			// server routes it to that one machine and nobody else, and
			// friendly fire has no say: shooting NPCs is not players hurting
			// each other.
			C_PedDamage out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].pedDamage;
			m_net.Send(out, CH_EVENT);
			break;
		}
		case CombatEvent::NPC_SHOT: {
			// One of our pedestrians fired. Unreliable: it is only drawn, and
			// only for somebody near enough to see it.
			if (!placed) {
				viewerCount = ViewerPositions(viewers, MAX_PLAYERS);
				placed      = true;
			}
			if (!game::NpcShotWorthSending(events[i].shot.origin, viewers, viewerCount))
				break;
			C_NpcShot out;
			InitHeader(out, WallClock::NowMs());
			out.pedNetId = events[i].npcNetId;
			out.body     = events[i].shot;
			m_net.Send(out, CH_SNAPSHOT);
			break;
		}
		case CombatEvent::NPC_DAMAGE: {
			// One of our pedestrians hit another player's copy here. The victim
			// applies it, the same way a hit of our own player's does.
			C_NpcDamage out;
			InitHeader(out, WallClock::NowMs());
			out.attackerPedNetId = events[i].npcNetId;
			out.body             = events[i].damage;
			m_net.Send(out, CH_EVENT);
			break;
		}
		case CombatEvent::DEATH:
			AnnounceDeath(events[i].deathAnimId);
			break;
		}
	}
}

// ---- time of day and weather -----------------------------------------------
//
// One player's game is the session's clock, and everyone else is nudged
// toward it. docs/protocol.md §2.7 is the argument for picking a player
// rather than letting the server keep a clock of its own; what matters here
// is that being corrected has to be cheaper than being wrong.
//
// So a correction is a jump, and the tolerance is what stops it happening.
// GTA III's clock runs on its own at the same rate on every machine, so two
// synced games stay synced; the drift that does show up comes from joining
// at a different hour, from a loading screen, or from the script moving the
// clock outright. All of those are far bigger than kClockToleranceMinutes
// and get fixed in one step, while the one or two minutes of lag that a
// packet is worth never moves anything.

int ClockDriftMinutes(uint8_t fromHour, uint8_t fromMinute, uint8_t toHour,
                      uint8_t toMinute) {
	constexpr int kDay  = 24 * 60;
	constexpr int kHalf = kDay / 2;

	const int from = (int(fromHour) % 24) * 60 + int(fromMinute) % 60;
	const int to   = (int(toHour) % 24) * 60 + int(toMinute) % 60;

	int drift = to - from;
	if (drift > kHalf)
		drift -= kDay;
	else if (drift < -kHalf)
		drift += kDay;
	return drift;
}

void Client::SetHost(uint8_t hostPlayerId) {
	const bool was = IsHost();
	m_hostPlayerId = hostPlayerId;
	if (was == IsHost())
		return;

	if (IsHost()) {
		Log("client: we're the host now, so our clock and our sky are the "
		    "session's");
		// If this machine spent any time as an ordinary client its sky is
		// pinned to whatever the last host reported, and a pinned sky never
		// rotates again. Hand it back or the session inherits one weather
		// type from the moment of the handover and keeps it forever.
		//
		// Only if we were the ones who pinned it. A machine that has been
		// the host since it joined may have a ForcedWeatherType its own
		// mission script set, and that one isn't ours to clear.
		if (m_weatherPinned && m_bridge.ReleaseWorldWeather) {
			m_bridge.ReleaseWorldWeather();
			m_weatherPinned = false;
		}
	} else {
		Log("client: player %u has the session clock", hostPlayerId);
	}
}

void Client::OnWorldState(const S_WorldState &pkt) {
	SetHost(pkt.hostPlayerId);
	ApplyWorldState(pkt.body);
	NoteServerTime(pkt.hdr.sendTimeMs);
}

// The trains and planes follow the server rather than the host, unlike the
// time of day. Nothing in any game moves this clock, so there is nothing a
// host would be the authority on, and the server's clock doesn't change hands
// when the host leaves. Stamped as it goes out (server.h,
// BroadcastWorldState), so the host gets the same treatment as everyone else.
void Client::NoteServerTime(uint32_t serverMs) {
	const bool had = m_sessionTime.Valid();
	m_sessionTime.AddSample(serverMs, WallClock::NowMs(), m_net.RoundTripMs());
	if (!had)
		Log("client: trains and planes follow the server's clock (round trip "
		    "%u ms)",
		    m_net.RoundTripMs());
}

// ---- pickups ---------------------------------------------------------------
//
// docs/pickups.md is the design. The three things this half is responsible
// for, and none of them is a decision about a pickup:
//
//   - getting a claim out of the detour and onto the wire, unqueued, because
//     a claim is only useful before the engine has awarded anything;
//   - reading S_PickupTaken as "ours" or "somebody else's", which is one
//     comparison against our own player id;
//   - turning a grant we could not consume back into a release, so the
//     session never holds a pickup nobody has.
//
// Who gets a pickup is the server's decision and what happens to it is the
// engine's. Neither is made here.

void Client::ClaimPickup(const PickupIdent &ident) {
	C_PickupClaim out;
	InitHeader(out, WallClock::NowMs());
	out.ident = ident;
	m_net.Send(out, CH_EVENT);
}

void Client::ReleasePickup(const PickupIdent &ident) {
	C_PickupRelease out;
	InitHeader(out, WallClock::NowMs());
	out.ident = ident;
	m_net.Send(out, CH_EVENT);
}

void Client::CollectedPickup(const PickupIdent &ident) {
	C_PickupCollected out;
	InitHeader(out, WallClock::NowMs());
	out.ident = ident;
	m_net.Send(out, CH_EVENT);
}

// ---------------------------------------------------------------------------
// Rampages - game/darkel.h is the design
// ---------------------------------------------------------------------------

void Client::RampageStarted(const RampageStartBody &body) {
	// Every machine's script starts the same frenzy in the same frame, so
	// every machine says so and the server keeps the first. The guard is not
	// about that - it is about a reconnect inside a running rampage, where
	// this client's own frenzy is still going and nothing on this machine
	// would otherwise stop it announcing one the session already has open.
	if (m_rampageRule == RAMPAGE_RULE_OFF || m_frenzyReported)
		return;
	m_frenzyReported = true;

	C_RampageStart out;
	InitHeader(out, WallClock::NowMs());
	out.body = body;
	m_net.Send(out, CH_EVENT);
}

void Client::RampageKilled(uint16_t model, uint8_t weapon, bool headshot) {
	// No open frenzy means nobody to tell. That is the normal case for a
	// client that joined mid-rampage: its own script never saw the pickup,
	// its own CDarkel is not running, and this is never reached.
	if (m_rampageRule == RAMPAGE_RULE_OFF || m_frenzyId == NO_FRENZY)
		return;

	C_RampageKill out;
	InitHeader(out, WallClock::NowMs());
	out.body.frenzyId = m_frenzyId;
	out.body.model    = model;
	out.body.weapon   = weapon;
	out.body.flags    = headshot ? uint8_t(RK_F_HEADSHOT) : uint8_t(0);
	m_net.Send(out, CH_EVENT);
}

void Client::RampageCarDestroyed(uint16_t model, const UnownedVehicleKey &key) {
	// Same two guards as a kill. With no open frenzy the car still counted on
	// our own engine, and nobody else is running one to count it against.
	if (m_rampageRule == RAMPAGE_RULE_OFF || m_frenzyId == NO_FRENZY)
		return;

	C_RampageCar out;
	InitHeader(out, WallClock::NowMs());
	out.body.frenzyId = m_frenzyId;
	out.body.model    = model;
	out.body.key      = key;
	out.body.key.pad  = 0;
	m_net.Send(out, CH_EVENT);
}

void Client::RampageEnded(uint8_t outcome) {
	if (m_rampageRule == RAMPAGE_RULE_OFF || m_frenzyId == NO_FRENZY)
		return;
	if (!IsRampageOutcome(outcome))
		return;

	C_RampageEnd out;
	InitHeader(out, WallClock::NowMs());
	out.body.frenzyId = m_frenzyId;
	out.body.outcome  = outcome;
	m_net.Send(out, CH_EVENT);
}

void Client::OnRampageOpen(const S_RampageOpen &pkt) {
	m_frenzyId = pkt.body.frenzyId;
	// The report has been answered. A second start for the same frenzy - a
	// reconnect, a script that somehow ran init_rampage twice - is refused by
	// the server anyway, but leaving this set would also stop the *next*
	// rampage being announced at all.
	m_frenzyReported = false;

	// Under the default rule this is the number the script already wrote and
	// the seam does nothing with it. Under `scaled` it is not, and the seam
	// writes it into CDarkel::KillsNeeded so that the HUD, the countdown and
	// the pass condition are all the session's from the first frame.
	if (m_bridge.ApplyRampageOpen)
		m_bridge.ApplyRampageOpen(pkt.body.killsNeeded, pkt.body.elapsedMs);
}

void Client::OnRampageKill(const S_RampageKill &pkt) {
	// Never us: the server leaves the reporter out, because their own engine
	// counted it before the packet was built. The guard is here for the same
	// reason OnPickupTaken's is - a packet that did arrive with our own id
	// would have us count one kill twice.
	if (pkt.byPlayer == m_localPlayerId)
		return;
	// A kill for a frenzy that is not the one we are in. Dropped rather than
	// applied: rampage.sc puts a failed rampage's pickup back within a frame
	// or two, so crediting a straggler to the next frenzy is a real risk
	// rather than a theoretical one.
	if (m_frenzyId == NO_FRENZY || pkt.body.frenzyId != m_frenzyId)
		return;
	if (m_bridge.CreditRampageKill)
		m_bridge.CreditRampageKill(pkt.body.model, pkt.body.weapon,
		                           (pkt.body.flags & RK_F_HEADSHOT) != 0);
}

void Client::OnRampageCar(const S_RampageCar &pkt) {
	// The same three drops as a kill. Whether a named car was already counted
	// here is the seam's question, not this one's: the answer depends on what
	// our own engine blew up, which only the seam sees.
	if (pkt.byPlayer == m_localPlayerId)
		return;
	if (m_frenzyId == NO_FRENZY || pkt.body.frenzyId != m_frenzyId)
		return;
	if (m_bridge.CreditRampageCar)
		m_bridge.CreditRampageCar(pkt.body.model, pkt.body.key);
}

void Client::OnRampageEnd(const S_RampageEnd &pkt) {
	if (m_frenzyId == NO_FRENZY || pkt.body.frenzyId != m_frenzyId)
		return;
	if (!IsRampageOutcome(pkt.body.outcome))
		return;

	m_frenzyId       = NO_FRENZY;
	m_frenzyReported = false;
	if (m_bridge.ApplyRampageVerdict)
		m_bridge.ApplyRampageVerdict(pkt.body.outcome);
}

// ---- the vote before a rampage (rampagevoteview.h, game/rampagevote.h) ------

void Client::OnRampageVote(const S_RampageVote &pkt) {
	const bool fresh = !m_rampageVote.seen || m_rampageVote.body.voteId != pkt.body.voteId ||
	                   m_rampageVote.body.state != pkt.body.state;
	m_rampageVote.OnVote(pkt.body, WallClock::NowMs());
	if (!fresh)
		return;

	const char *nick = NickFor(pkt.body.starterId);
	if (pkt.body.state == RAMPAGE_VOTE_OPEN)
		Log("client: rampage vote %u open, %s touched a skull (%u/%u yes, %u needed, %u ms)",
		    pkt.body.voteId, nick ? nick : "?", pkt.body.yes, pkt.body.voters, pkt.body.needed,
		    pkt.body.msLeft);
	else
		Log("client: rampage vote %u %s (%u/%u yes)", pkt.body.voteId,
		    pkt.body.state == RAMPAGE_VOTE_PASSED      ? "passed"
		    : pkt.body.state == RAMPAGE_VOTE_CANCELLED ? "called off"
		                                               : "failed",
		    pkt.body.yes, pkt.body.voters);
}

void Client::OnRampageTeleport(const S_RampageTeleport &pkt) {
	// Never the toucher: he is where everybody is going.
	if (pkt.body.starterId == m_localPlayerId)
		return;
	m_teleport        = pkt.body;
	m_teleportWaiting = true;
}

bool Client::CastRampageVote(bool yes) {
	if (!m_rampageVote.MayCast(m_localPlayerId))
		return false;
	C_RampageVote out;
	InitHeader(out, WallClock::NowMs());
	out.voteId = m_rampageVote.body.voteId;
	out.yes    = yes ? 1 : 0;
	m_net.Send(out, CH_EVENT);
	m_rampageVote.NoteCast(yes);
	Log("client: voted %s in rampage vote %u", yes ? "yes" : "no", out.voteId);
	return true;
}

bool Client::TakeRampageTeleport(RampageTeleportBody &out) {
	if (!m_teleportWaiting)
		return false;
	m_teleportWaiting = false;
	out               = m_teleport;
	return true;
}

void Client::ReportRampageArrival(uint8_t voteId, uint8_t result) {
	C_RampageArrived out;
	InitHeader(out, WallClock::NowMs());
	out.voteId = voteId;
	out.result = result;
	m_net.Send(out, CH_EVENT);
}

const char *Client::NickFor(uint8_t playerId) const {
	if (playerId == m_localPlayerId)
		return m_localNick.c_str();
	if (playerId < MAX_PLAYERS && m_players[playerId].active)
		return m_players[playerId].nick.c_str();
	return nullptr;
}

void Client::OnPickupGrant(const S_PickupGrant &pkt) {
	// A reservation, not a collection. The seam unblocks it and the engine
	// decides on its own next pass, through its own touch test, its own
	// CanBePickedUp and its own award switch - so the reward, the sound, the
	// pad shake and the respawn timer are all the game's, exactly as in
	// single player. Whether it took it comes back as CollectedPickup.
	if (m_bridge.PickupGrantedToUs && !m_bridge.PickupGrantedToUs(pkt.ident))
		ReleasePickup(pkt.ident);
}

void Client::OnPickupTaken(const S_PickupTaken &pkt) {
	// Never us: the server leaves the collector out of this broadcast,
	// because their own engine already removed their copy. The guard is here
	// anyway, because a packet that does reach us with our own id would
	// otherwise have us replay a removal our engine has already done.
	if (pkt.playerId == m_localPlayerId)
		return;

	if (m_bridge.PickupTakenByOther)
		m_bridge.PickupTakenByOther(pkt.ident);
}

bool Client::DroppedPickup(const PickupDropBody &drop) {
	// The callbacks are wired once at boot and the client reconnects for as
	// long as the game runs, so "wired" is not "connected". Without this the
	// seam would announce a drop into a dead socket and log that the session
	// had been told.
	if (!IsConnected())
		return false;

	C_PickupDrop out;
	InitHeader(out, WallClock::NowMs());
	out.body = drop;
	m_net.Send(out, CH_EVENT);
	return true;
}

void Client::OnPickupDrop(const S_PickupDrop &pkt) {
	// Never us. The server leaves the owner out of the relay because their
	// own engine made the pickup - that is what produced the message. The
	// guard is here anyway, because building a second copy of our own drop
	// would put two pickups of the same model inside the ident tolerance and
	// neither would ever be claimable again.
	if (pkt.playerId == m_localPlayerId)
		return;

	if (m_bridge.PickupDropped)
		m_bridge.PickupDropped(pkt.body);
}

// Only CoopIII's own peds are replicas, and both rosters are small. A remote
// player's ped and an ambient replica get the same answer because they are
// the same kind of thing here: an entity whose decisions belong to another
// machine, which under docs/roadmap.md 5 includes deciding what it drops when
// it dies.
bool Client::IsReplicatedPed(int32_t pedRef) const {
	// -1 is "the engine could not name it", which is not a replica. Answering
	// true here would suppress a drop this machine is entitled to make.
	if (pedRef < 0)
		return false;

	for (const RemotePlayer &p : m_players)
		if (p.active && p.poolHandle == pedRef)
			return true;

	for (const RemoteAmbientPed &p : m_peds)
		if (p.active && p.poolHandle == pedRef)
			return true;

	return false;
}

// The same question as IsReplicatedPed, about the other roster. Both lists
// are walked rather than indexed for the same reason: MAX_REMOTE_VEHICLES and
// MAX_REMOTE_CARS are small, this runs once per break, and a second index of
// pool refs is a second thing to keep in step with every spawn and despawn.
bool Client::IsReplicatedVehicle(int32_t vehRef) const {
	if (vehRef < 0)
		return false;

	for (const RemoteVehicle &v : m_vehicles)
		if (v.active && v.poolHandle == vehRef)
			return true;

	for (const RemoteAmbientCar &c : m_cars)
		if (c.active && c.poolHandle == vehRef)
			return true;

	// A replica of somebody else's helicopter is a vehicle too, and one this
	// machine only watches.
	return m_helis.IsReplica(vehRef);
}

bool Client::ReportObjectBroken(const ObjectBreakBody &body) {
	if (!IsConnected())
		return false;

	C_ObjectBroken out;
	InitHeader(out, WallClock::NowMs());
	out.body = body;
	m_net.Send(out, CH_EVENT);
	return true;
}

void Client::OnObjectBroken(const S_ObjectBroken &pkt) {
	// Never us: the server leaves the reporter out of the relay because their
	// own engine is what produced the message. The guard is here anyway,
	// because replaying our own break would push a
	// DAMAGE_EFFECT_CHANGE_THEN_SMASH object one step further than the
	// reporter's own engine took it.
	if (pkt.playerId == m_localPlayerId)
		return;

	if (m_bridge.ObjectBroken)
		m_bridge.ObjectBroken(pkt.body);
}

bool Client::ReportObjectSettled(const ObjectRestBody &body) {
	if (!IsConnected())
		return false;

	C_ObjectSettled out;
	InitHeader(out, WallClock::NowMs());
	out.body = body;
	m_net.Send(out, CH_EVENT);
	return true;
}

void Client::OnObjectSettled(const S_ObjectSettled &pkt) {
	// Never us, for a plainer reason than the break's: our own object is
	// already lying exactly where this packet says, because this packet is a
	// copy of where it is.
	if (pkt.playerId == m_localPlayerId)
		return;

	if (m_bridge.ObjectSettled)
		m_bridge.ObjectSettled(pkt.body);
}

void Client::OnPickupDenied(const S_PickupDenied &pkt) {
	// Nothing to undo. The engine never saw an object there, which is the
	// whole reason the arbitration happens before the award.
	if (m_bridge.PickupDenied)
		m_bridge.PickupDenied(pkt.ident);
}

void Client::ApplyWorldState(const WorldStateBody &body) {
	// The host is the original, not a copy of one.
	if (IsHost())
		return;

	// eWeatherType indexes arrays inside CWeather with no bounds check of
	// its own, and CClock only ever tests its hour for >= 24 after an
	// increment, so a value out of range here doesn't self-correct. Nothing
	// on the wire is trusted into either.
	constexpr uint8_t WEATHER_TOTAL = 4;

	if (body.hour <= 23 && body.minute <= 59 && m_bridge.SampleWorld &&
	    m_bridge.ApplyWorldTime) {
		WorldState mine;
		if (m_bridge.SampleWorld(mine)) {
			const int drift =
			    ClockDriftMinutes(mine.hour, mine.minute, body.hour, body.minute);
			if (drift > kClockToleranceMinutes || drift < -kClockToleranceMinutes) {
				Log("client: clock is %d minute(s) off the session, moving "
				    "%02u:%02u -> %02u:%02u",
				    drift, mine.hour, mine.minute, body.hour, body.minute);
				m_bridge.ApplyWorldTime(body.hour, body.minute);
			}
		}
	}

	// Weather is written every time rather than on change. The "only on
	// change" rule the ped and vehicle code follows is there because those
	// calls rebuild models and restart animation blends; this one is a
	// couple of 16-bit stores, and writing it unconditionally means the
	// local weather rotation can never quietly win a race against us.
	if (body.weather < WEATHER_TOTAL && body.weatherOld < WEATHER_TOTAL &&
	    m_bridge.ApplyWorldWeather) {
		m_bridge.ApplyWorldWeather(body.weather, body.weatherOld);
		m_weatherPinned = true;
	}
}

void Client::SendLocalWorld() {
	if (!IsHost() || !m_bridge.SampleWorld) {
		m_worldSendNow = false;
		return;
	}
	// A sky cheat skips the queue once, and does not reset the limiter: the
	// regular packet still comes on its own beat.
	if (!m_worldSendNow && !m_worldRate.Ready(WallClock::NowMs()))
		return;

	WorldState mine;
	if (!m_bridge.SampleWorld(mine))
		return;   // no world to report - a loading screen, the menu
	m_worldSendNow = false;

	C_WorldState out;
	InitHeader(out, WallClock::NowMs());
	out.body.hour       = mine.hour;
	out.body.minute     = mine.minute;
	out.body.weather    = mine.weather;
	out.body.weatherOld = mine.weatherOld;
	m_net.Send(out, CH_EVENT);
}

// ---- cheats (game/cheats.h, docs/cheats.md) ----------------------------------
//
// The seam decides where a typed cheat runs; this half only carries it. The
// one thing decided here is the host's own sky: nobody needs a packet for it,
// they need the world packet it changed, and they need it now rather than
// whenever the 1 Hz limiter next lets one out.

void Client::PushCheatSession() {
	if (!m_bridge.SetCheatSession)
		return;
	// "In a session" is "welcomed and not cleared since". PreFrame clears the
	// roster the frame the connection drops, so this goes false with it.
	m_bridge.SetCheatSession(m_localPlayerId != INVALID_PLAYER, IsHost(),
	                         m_cheatRule);
}

void Client::SendLocalCheats() {
	if (!m_bridge.DrainLocalCheats)
		return;

	constexpr uint8_t MAX_PER_FRAME = 4;
	CheatBody         typed[MAX_PER_FRAME];
	const uint8_t     n = m_bridge.DrainLocalCheats(typed, MAX_PER_FRAME);
	for (uint8_t i = 0; i < n; ++i) {
		const uint8_t route = CheatRouteOf(typed[i].cheat);
		if (route == CHEAT_ROUTE_HOST && IsHost()) {
			m_worldSendNow = true;
			continue;
		}
		if (route == CHEAT_ROUTE_LOCAL)
			continue;   // the seam never queues one; nothing to carry if it did

		C_Cheat out;
		InitHeader(out, WallClock::NowMs());
		out.body = typed[i];
		m_net.Send(out, CH_EVENT);
		Log("client: cheat %u went to %s", static_cast<unsigned>(typed[i].cheat),
		    route == CHEAT_ROUTE_HOST ? "the host" : "everybody");
	}
}

void Client::OnCheat(const S_Cheat &pkt) {
	// Our own, relayed back. The server sends it to everybody *but* the
	// typist, so this is the guard for the day something does not.
	if (m_localPlayerId != INVALID_PLAYER && pkt.playerId == m_localPlayerId)
		return;

	const uint8_t id    = pkt.body.cheat;
	const uint8_t state = pkt.body.state;

	// The server's three checks again. A client that trusted them would take a
	// riot from a server configured with cheats off, or one too old to know.
	if (!CheatAllowed(m_cheatRule, id) || !IsValidCheatState(id, state)) {
		Log("client: dropped cheat %u (state %u) from player %u - this session "
		    "does not allow it", static_cast<unsigned>(id),
		    static_cast<unsigned>(state), static_cast<unsigned>(pkt.playerId));
		return;
	}
	const uint8_t route = CheatRouteOf(id);
	if (route == CHEAT_ROUTE_HOST && !IsHost()) {
		// Only possible across a host change: the server picked us, and by the
		// time it arrived somebody else had the clock. Theirs is the sky now,
		// and the typist can type it again.
		Log("client: a sky cheat reached us after we stopped being the host; "
		    "dropped");
		return;
	}
	if (!m_bridge.ApplyRoutedCheat)
		return;

	const char *who = pkt.playerId < MAX_PLAYERS && m_players[pkt.playerId].active
	                      ? m_players[pkt.playerId].nick.c_str()
	                      : "somebody who has since left";
	Log("client: %s used cheat %u; bringing it about here", who,
	    static_cast<unsigned>(id));
	m_bridge.ApplyRoutedCheat(id, state);
	if (route == CHEAT_ROUTE_HOST)
		m_worldSendNow = true;
}

// ---- garages, doors and the Pay'n'Spray -------------------------------------
//
// game/garage.h is the design and the argument. This half is the roster and
// nothing else, which is why it is here and not in the engine seam: it is one
// OR over eight numbers, and tools/clienttest can check it without a game.

uint32_t Client::RemoteGarageMask() const {
	// The union, and deliberately not "the newest report wins".
	//
	// A door being open is a *level*, true for as long as somebody is
	// standing there, and not an event that happens once. Last-writer-wins
	// would mean the first player to walk away from a shared safehouse shuts
	// the door on the second, and roadmap.md §5.8's "first report wins" -
	// which is right for a car being destroyed - would mean the door never
	// closes at all. An OR is the only reduction that is correct for both
	// directions and for any number of players.
	//
	// The local slot is skipped rather than left at zero, because a client
	// that is relayed its own packet back (nothing does that today, and
	// nothing should have to promise not to) would otherwise feed its own
	// opinion back in as somebody else's and hold its own doors.
	uint32_t mask = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == m_localPlayerId)
			continue;
		mask |= m_garageMasks[id];
	}
	return mask;
}

void Client::UpdateGarages() {
	if (!m_bridge.ApplyRemoteGarages || !m_bridge.SampleLocalGarages)
		return;

	const uint32_t held = RemoteGarageMask();
	m_bridge.ApplyRemoteGarages(held);
	if (held != 0 && !m_saidGarageHeld) {
		m_saidGarageHeld = true;
		Log("garage: another player has a garage away from rest (mask 0x%08X); "
		    "holding ours to match",
		    static_cast<unsigned>(held));
	}

	// Read back what our own state machine decided this frame. The seam
	// samples it from inside CGarage::Update, at the one moment the state
	// byte is the engine's own opinion rather than partly CoopIII's, so this
	// is a read of a number the frame already produced and not a second
	// sweep over the array.
	uint32_t mine = 0;
	if (!m_bridge.SampleLocalGarages(mine))
		return;   // no world yet - the menu, a loading screen
	m_localGarageMask = mine;

	if (mine == m_sentGarageMask)
		return;

	C_GarageState out;
	InitHeader(out, WallClock::NowMs());
	out.body.deviating = mine;
	m_net.Send(out, CH_EVENT);
	m_sentGarageMask = mine;
}

void Client::OnGarageState(const S_GarageState &pkt) {
	if (pkt.playerId >= MAX_PLAYERS || pkt.playerId == m_localPlayerId)
		return;
	m_garageMasks[pkt.playerId] = pkt.body.deviating;
}

void Client::SendLocalResprays() {
	if (!m_bridge.DrainLocalResprays)
		return;

	LocalRespray done[4];
	const uint8_t n = m_bridge.DrainLocalResprays(done, 4);
	for (uint8_t i = 0; i < n; ++i) {
		C_Respray out;
		InitHeader(out, WallClock::NowMs());
		out.body.vehicleNetId = m_localVehicleNetId;
		out.body.garage       = done[i].garage;
		out.body.colour1      = done[i].colour1;
		out.body.colour2      = done[i].colour2;
		out.body.pad[0] = out.body.pad[1] = out.body.pad[2] = 0;
		m_net.Send(out, CH_EVENT);

		// And the dents the spray shop just took off, through the damage
		// work's own packet rather than a second thing that repairs cars.
		//
		// The respray above repaints an observer's copy and calls
		// CAutomobile::Fix on it, so the car on screen comes out clean either
		// way. What this is for is the bookkeeping either side of that, which
		// Fix does not touch and which is what makes the dents come back:
		// the server's record would go on backfilling them to joiners, the
		// observer's row would re-apply them the next time the car is
		// respawned, and our own high-water mark would swallow every dent
		// below it for the rest of the session.
		//
		// Sent after the respray, not before, because the two are ordered on
		// CH_EVENT and a clear that arrives ahead of the repair is a clear of
		// something that has not happened yet.
		if (m_localVehicleNetId != INVALID_NETID) {
			C_VehicleDamage clear;
			InitHeader(clear, WallClock::NowMs());
			clear.body        = VehicleDamageBody{};
			clear.body.netId  = m_localVehicleNetId;
			clear.body.panels = VEH_DAMAGE_RESET;
			clear.body.doors  = 0;
			m_net.Send(clear, CH_EVENT);

			// Our own baseline goes with it. Without this the next scrape is
			// compared against the pre-respray word and never reported.
			m_sentDamagePanels = 0;
			m_sentDamageDoors  = 0;
			m_haveSentDamage   = true;
		}
	}
}

void Client::OnRespray(const S_Respray &pkt) {
	if (pkt.playerId == m_localPlayerId)
		return;
	if (!m_bridge.ApplyRemoteRespray)
		return;
	// Null when the session has no row for that car - somebody drove an
	// unclaimed traffic car in. The seam is still called, because the
	// decision about what "no car" means belongs on the engine side with the
	// rest of it, and because a seam that is only called when there is
	// something to do is a seam nobody can log.
	RemoteVehicle *const car = pkt.body.vehicleNetId == INVALID_NETID
	                               ? nullptr
	                               : VehicleSlot(pkt.body.vehicleNetId, false);
	m_bridge.ApplyRemoteRespray(car, pkt.body);
}

// ---- vehicles --------------------------------------------------------------

const RemoteVehicle *Client::VehicleByNetId(uint16_t netId) const {
	for (const RemoteVehicle &v : m_vehicles)
		if (v.active && v.netId == netId)
			return &v;
	return nullptr;
}

uint8_t Client::VehicleCount() const {
	uint8_t n = 0;
	for (const RemoteVehicle &v : m_vehicles)
		if (v.active)
			++n;
	return n;
}

RemoteVehicle *Client::VehicleSlot(uint16_t netId, bool createIfMissing) {
	for (RemoteVehicle &v : m_vehicles)
		if (v.active && v.netId == netId)
			return &v;
	if (!createIfMissing)
		return nullptr;
	for (RemoteVehicle &v : m_vehicles)
		if (!v.active) {
			v       = RemoteVehicle{};
			v.active = true;
			v.netId  = netId;
			return &v;
		}
	// Full. Not fatal, and not worth a log line per packet either - the
	// vehicle just isn't shown, and the next despawn frees a slot.
	return nullptr;
}

bool Client::DrivenLocally(const RemoteVehicle &vehicle) const {
	return m_localVehicleNetId != INVALID_NETID && vehicle.netId == m_localVehicleNetId;
}

bool Client::VehicleIsOursToDrive(const RemoteVehicle &vehicle) const {
	if (DrivenLocally(vehicle))
		return true;
	if (vehicle.surrendered)
		return false;
	return m_bridge.LocalDrivesVehicle && m_bridge.LocalDrivesVehicle(vehicle);
}

void Client::NoteVehicleDriverChanged(RemoteVehicle &vehicle,
                                      uint8_t driverPlayerId) {
	if (driverPlayerId == m_localPlayerId) {
		// It is ours again - we jacked it back, or the reply to our own claim
		// came round. Whatever the engine says now is the truth again.
		vehicle.surrendered   = false;
		vehicle.surrenderDone = false;
		return;
	}

	// Only a loss if we are actually sitting at its wheel. A car we were
	// watching all along changing driver is the ordinary case and needs
	// nothing: the guards already say it is not ours.
	if (!m_bridge.LocalDrivesVehicle || !m_bridge.LocalDrivesVehicle(vehicle))
		return;
	if (vehicle.surrendered)
		return;   // already said; the handover runs once

	vehicle.surrendered   = true;
	vehicle.surrenderDone = false;

	// And we are not its owner in the session either, whatever we thought.
	// The server sends an S_ExitVehicle to the loser of a jack before the
	// enter that names the winner, so this is normally already done; it is
	// repeated here because the two must never disagree, and because a client
	// talking to a server whose arbitration is missing has to survive it -
	// left set, m_localVehicleNetId would keep DrivenLocally true and this
	// machine would go on refusing every snapshot the new owner sends, which
	// is the bug in one line.
	if (m_localVehicleNetId == vehicle.netId) {
		m_localVehicleNetId   = INVALID_NETID;
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;
	}

	Log("client: player %u has taken vehicle %u off us - our engine still has "
	    "us at the wheel, so the seat is being handed over and the car becomes "
	    "one we watch",
	    static_cast<unsigned>(driverPlayerId), vehicle.netId);
}

// Which of the session's cars is that pool slot, if any.
//
// Matched on the engine's own reference, not on model and position. Identity
// has to be exact here: two identical parked cars side by side are an
// ordinary sight in Liberty City, and picking the wrong one would register
// the session's car under a second netId and leave the real one pinned - the
// same bug, arrived at more cleverly.
RemoteVehicle *Client::VehicleByPoolHandle(int32_t handle) {
	if (handle < 0)
		return nullptr;
	for (RemoteVehicle &v : m_vehicles)
		if (v.active && v.poolHandle >= 0 && v.poolHandle == handle)
			return &v;
	return nullptr;
}

RemoteVehicle *Client::ObservedVehicleWeAreDriving() {
	if (!m_bridge.SampleLocalVehicleHandle)
		return nullptr;
	return VehicleByPoolHandle(m_bridge.SampleLocalVehicleHandle());
}

void Client::OnVehicleSpawn(const S_VehicleSpawn &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/true);
	if (!v)
		return;

	v->modelId = pkt.modelId;
	v->colour1 = pkt.colour1;
	v->colour2 = pkt.colour2;
	// Only useful before the car exists: SpawnRemoteVehicle forces them
	// through the engine's override while it constructs the car, and nothing
	// can change them afterwards. game/vehicle.h, "Extras".
	v->extra1  = pkt.extra1;
	v->extra2  = pkt.extra2;

	// The spawn packet carries a position, so unlike a player join it's
	// enough on its own to place the vehicle. Seeding `last` from it lets
	// the spawn happen before the first state snapshot arrives, without
	// repeating the ped mistake of creating an entity at the origin.
	//
	// Health and flags come off the packet as of protocol 9, and used to be
	// the constant 1000 and nothing. That constant is the bug the owner
	// found from the other end: a car he had blown up came back to a late
	// joiner in showroom condition, drivable-looking and not drivable,
	// because this line said the car was fine and the live stream then wrote
	// the real health onto a model that had already been built healthy. The
	// spawn has to describe the car's condition, not just its identity.
	v->last.netId  = pkt.netId;
	v->last.pos    = pkt.pos;
	v->last.rot    = pkt.rot;
	v->last.health = pkt.health;
	v->last.flags  = pkt.flags;
	v->haveState   = true;
	v->spawnPending = true;

	// The car we were in when the last session ended, and are still in: the
	// same CVehicle, which the ended session handed to our engine, so it is
	// ours now and there is nothing to build. The claim that follows names it
	// by this netId, and the session makes us its driver again.
	// Not if another row already has that car: a claim that went out first
	// has given it a number of its own.
	if (pkt.netId == m_rejoinNetId && pkt.modelId == m_rejoinModel && v->poolHandle < 0 &&
	    WaitingForRejoin() && !VehicleByPoolHandle(m_rejoinHandle) &&
	    m_bridge.SampleLocalVehicleHandle &&
	    m_bridge.SampleLocalVehicleHandle() == m_rejoinHandle) {
		v->poolHandle   = m_rejoinHandle;
		v->ours         = true;
		v->spawnPending = false;
		if (m_bridge.AdoptClaimedVehicle)
			m_bridge.AdoptClaimedVehicle(*v);
		Log("client: back in the session in the car we were in (vehicle %u); taking "
		    "it up again rather than building a second one", pkt.netId);
		m_rejoinAdoptedNetId = pkt.netId;
		m_rejoinNetId        = INVALID_NETID;
		m_rejoinHandle       = -1;
		return;
	}
	if (pkt.netId == m_rejoinNetId) {
		// Out of it since, or a different car under the same number: built
		// the ordinary way, and nothing more to wait for.
		ForgetRejoin();
	}

	if (m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.modelId);

	Log("client: vehicle %u joined (model %u, %.0f hp%s, extras %d/%d)", pkt.netId,
	    pkt.modelId, pkt.health, (pkt.flags & VEH_WRECKED) ? ", wrecked" : "",
	    static_cast<int>(pkt.extra1), static_cast<int>(pkt.extra2));
}

// The buffer is ordered on send times, and two machines' send times have
// nothing to do with each other. Kept across a change of hands, a car taken
// over by a machine whose game started later had every new snapshot older
// than the last one its previous driver sent, and Push dropped them all: the
// car stood still here for as long as the two games' start times were apart.
//
// The whole buffer, playback clock included: a clock left running would go on
// easing in the old timebase and play the new driver's first samples out of
// step with them.
void Client::OnReportersClock(RemoteVehicle &vehicle, uint8_t reporterId) {
	if (vehicle.reporterPlayerId == reporterId)
		return;
	if (vehicle.reporterPlayerId != 0xFF)
		vehicle.interp = VehicleInterpBuffer{};
	vehicle.reporterPlayerId = reporterId;
}

bool Client::WaitingForRejoin() {
	if (m_rejoinUntilMs == 0)
		return false;
	if (static_cast<int32_t>(WallClock::NowMs() - m_rejoinUntilMs) >= 0) {
		ForgetRejoin();
		return false;
	}
	return true;
}

void Client::ForgetRejoin() {
	m_rejoinNetId        = INVALID_NETID;
	m_rejoinModel        = 0;
	m_rejoinHandle       = -1;
	m_rejoinUntilMs      = 0;
	m_rejoinAdoptedNetId = INVALID_NETID;
}

// Somebody else has been driving the car we were in since we dropped, or our
// old connection still is. It is theirs; the car we are sitting in is only our
// copy of it, so the row goes back to being one to build and ours gets claimed
// as a new car.
void Client::LetGoOfRejoinedCar(RemoteVehicle &vehicle, uint8_t driverPlayerId) {
	m_rejoinAdoptedNetId = INVALID_NETID;
	if (!vehicle.ours || vehicle.poolHandle < 0 || DrivenLocally(vehicle))
		return;
	if (m_bridge.ReleaseOwnVehicle)
		m_bridge.ReleaseOwnVehicle(vehicle);
	vehicle.poolHandle    = -1;
	vehicle.ours          = false;
	vehicle.spawnPending  = true;
	vehicle.surrendered   = false;
	vehicle.surrenderDone = false;
	vehicle.interp.Clear();
	if (m_bridge.RequestModel)
		m_bridge.RequestModel(vehicle.modelId);
	Log("client: player %u has vehicle %u in the session now; the car we are in "
	    "stays ours and gets a number of its own",
	    static_cast<unsigned>(driverPlayerId), vehicle.netId);
}

// Every remote player on foot whose ped we have, then as many of the session
// cars we only watch as still fit, taking turns when there are more.
uint8_t Client::BuildDesyncProbe(C_DesyncProbe &out) {
	InitHeader(out, WallClock::NowMs());
	uint8_t n = 0;

	if (m_bridge.SampleRemotePedPosition)
		for (const RemotePlayer &p : m_players) {
			if (n >= DESYNC_PROBE_ROWS)
				break;
			if (!p.active || p.poolHandle < 0 || p.playerId == m_localPlayerId)
				continue;
			uint32_t at = 0;
			Vec3     pos{};
			if (!p.interp.RenderedAt(at) || !m_bridge.SampleRemotePedPosition(p, pos))
				continue;
			out.rows[n].netId = p.netId;
			out.rows[n].atMs  = at;
			out.rows[n].pos   = pos;
			++n;
		}

	// Then everything else, one index space over the three rosters so they
	// share what is left of the packet fairly: session cars, traffic, peds.
	const uint32_t total = static_cast<uint32_t>(MAX_REMOTE_VEHICLES + MAX_REMOTE_CARS +
	                                             MAX_REMOTE_PEDS);
	uint32_t next = m_probeCarCursor % total;
	for (uint32_t k = 0; k < total && n < DESYNC_PROBE_ROWS; ++k) {
		const uint32_t i  = (m_probeCarCursor + k) % total;
		uint16_t       id = INVALID_NETID;
		uint32_t       at = 0;
		Vec3           pos{};
		if (i < MAX_REMOTE_VEHICLES) {
			RemoteVehicle &v = m_vehicles[i];
			if (!v.active || v.poolHandle < 0 || v.destroyed || !m_bridge.SampleObservedVehicle)
				continue;
			if (VehicleIsOursToDrive(v) || HaveCustodyOf(v))
				continue;
			VehicleStateBody body{};
			if (!v.interp.RenderedAt(at) || !m_bridge.SampleObservedVehicle(v, body))
				continue;
			id  = v.netId;
			pos = body.pos;
		} else if (i < MAX_REMOTE_VEHICLES + MAX_REMOTE_CARS) {
			const RemoteAmbientCar &car = m_cars[i - MAX_REMOTE_VEHICLES];
			if (!car.active || car.poolHandle < 0 || car.destroyed || !m_bridge.SampleReplicaPosition)
				continue;
			// Ours to drive until the promotion lands, and not a copy then.
			if (m_bridge.LocalDrivesAmbientCar && m_bridge.LocalDrivesAmbientCar(car))
				continue;
			if (!car.interp.RenderedAt(at) ||
			    !m_bridge.SampleReplicaPosition(car.poolHandle, true, pos))
				continue;
			id = car.netId;
		} else {
			const RemoteAmbientPed &ped = m_peds[i - MAX_REMOTE_VEHICLES - MAX_REMOTE_CARS];
			if (!ped.active || ped.poolHandle < 0 || ped.dead || ped.Seated() ||
			    !m_bridge.SampleReplicaPosition)
				continue;
			if (!ped.interp.RenderedAt(at) ||
			    !m_bridge.SampleReplicaPosition(ped.poolHandle, false, pos))
				continue;
			id = ped.netId;
		}
		out.rows[n].netId = id;
		out.rows[n].atMs  = at;
		out.rows[n].pos   = pos;
		++n;
		next = (i + 1) % total;
	}
	m_probeCarCursor = next;

	out.count = n;
	return n;
}

void Client::SendDesyncProbe(uint32_t nowMs) {
	if (m_localPlayerId >= MAX_PLAYERS)
		return;
	if (m_lastProbeMs != 0 && nowMs - m_lastProbeMs < DESYNC_PROBE_MS)
		return;
	m_lastProbeMs = nowMs ? nowMs : 1;

	C_DesyncProbe out;
	if (BuildDesyncProbe(out) == 0)
		return;
	m_net.Send(out, CH_SNAPSHOT);
}

void Client::OnDesyncReport(const S_DesyncReport &pkt) {
	const uint32_t now   = WallClock::NowMs();
	const uint8_t  count = pkt.count < DESYNC_PROBE_ROWS ? pkt.count : DESYNC_PROBE_ROWS;

	uint8_t  compared = 0;
	uint16_t worstCm = 0, worstNetId = INVALID_NETID;
	for (uint8_t i = 0; i < count; ++i) {
		const DesyncReportRow &row = pkt.rows[i];
		if (row.offCm == DESYNC_UNKNOWN || row.netId == INVALID_NETID)
			continue;
		bool found = false;
		for (RemotePlayer &p : m_players)
			if (p.active && p.netId == row.netId && p.playerId != m_localPlayerId) {
				p.offCm   = row.offCm;
				p.offAtMs = now;
				found     = true;
				break;
			}
		if (!found)
			if (RemoteVehicle *v = VehicleSlot(row.netId, false)) {
				v->offCm   = row.offCm;
				v->offAtMs = now;
				found      = true;
			}
		if (!found)
			if (RemoteAmbientCar *car = AmbientCarByNetId(row.netId)) {
				car->offCm   = row.offCm;
				car->offAtMs = now;
				found        = true;
			}
		if (!found)
			if (RemoteAmbientPed *ped = AmbientPedByNetId(row.netId)) {
				ped->offCm   = row.offCm;
				ped->offAtMs = now;
				found        = true;
			}
		if (!found)
			continue;
		++compared;
		if (worstNetId == INVALID_NETID || row.offCm > worstCm) {
			worstCm    = row.offCm;
			worstNetId = row.netId;
		}
	}
	if (compared == 0)
		return;

	if (!m_saidDesyncReport) {
		m_saidDesyncReport = true;
		Log("client: took our first desync report: %u of our copies compared, the "
		    "furthest %.1f m from its owner", compared, worstCm / 100.0f);
	}
	if (worstCm < DESYNC_NOTE_CM)
		return;
	if (m_desyncSaidAtMs != 0 && now - m_desyncSaidAtMs < DESYNC_NOTE_EVERY_MS)
		return;
	m_desyncSaidAtMs = now ? now : 1;
	for (const RemotePlayer &p : m_players)
		if (p.active && p.netId == worstNetId) {
			Log("client: the server says our copy of %s is %.1f m from where they "
			    "were at that instant", p.nick.c_str(), worstCm / 100.0f);
			return;
		}
	const char *what = AmbientPedByNetId(worstNetId)   ? "pedestrian"
	                   : AmbientCarByNetId(worstNetId) ? "traffic car"
	                                                   : "vehicle";
	Log("client: the server says our copy of %s %u is %.1f m from where its owner "
	    "had it at that instant", what, worstNetId, worstCm / 100.0f);
}

uint16_t Client::DesyncOf(uint8_t playerId) const {
	if (playerId >= MAX_PLAYERS)
		return DESYNC_UNKNOWN;
	const RemotePlayer &p = m_players[playerId];
	if (!p.active || p.offCm == DESYNC_UNKNOWN || WallClock::NowMs() - p.offAtMs > DESYNC_FRESH_MS)
		return DESYNC_UNKNOWN;
	return p.offCm;
}

uint16_t Client::WorstCopyDesync(uint16_t &netId, const char *&what) const {
	const uint32_t now   = WallClock::NowMs();
	uint16_t       worst = DESYNC_UNKNOWN;
	netId                = INVALID_NETID;
	what                 = "";
	auto consider = [&](bool active, uint16_t offCm, uint32_t offAtMs, uint16_t id,
	                    const char *kind) {
		if (!active || offCm == DESYNC_UNKNOWN || now - offAtMs > DESYNC_FRESH_MS)
			return;
		if (worst == DESYNC_UNKNOWN || offCm > worst) {
			worst = offCm;
			netId = id;
			what  = kind;
		}
	};
	for (const RemoteVehicle &v : m_vehicles)
		consider(v.active, v.offCm, v.offAtMs, v.netId, "vehicle");
	for (const RemoteAmbientCar &car : m_cars)
		consider(car.active, car.offCm, car.offAtMs, car.netId, "traffic car");
	for (const RemoteAmbientPed &ped : m_peds)
		consider(ped.active, ped.offCm, ped.offAtMs, ped.netId, "pedestrian");
	return worst;
}

void Client::OnVehicleDespawn(const S_VehicleDespawn &pkt) {
	// Nobody is left waiting to be put in it. OnVehicleBlowUp does the same: a
	// standing instruction naming a netId the session no longer has would be
	// retried against nothing. Before the roster lookup, since an enter can
	// name a car whose spawn hasn't arrived and now never will.
	for (RemotePlayer &p : m_players)
		if (p.active && p.seatVehicleNetId == pkt.netId)
			p.seatVehicleNetId = INVALID_NETID;

	// The session only lets go of a car nobody is in, but a release can race
	// the local player getting in. If it did, the car he's in no longer has a
	// netId: forget the old one so SendLocalVehicle claims it afresh, as the
	// unknown car it now is, instead of streaming snapshots the server drops.
	// DespawnRemoteVehicle keeps the car in that case (game/carlife.h).
	if (m_localVehicleNetId == pkt.netId) {
		m_localVehicleNetId   = INVALID_NETID;
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;
	}

	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/false);
	if (!v)
		return;

	// Everyone gets out before the car goes. See UnseatPlayer.
	for (RemotePlayer &p : m_players)
		if (p.active && p.InvolvedWith(pkt.netId))
			UnseatPlayer(p);

	// Not one of our own. The session dropping a car it can no longer track
	// is not the same statement as "this CVehicle should be destroyed", and
	// for a car our engine made it is not ours to make. See RemoteVehicle::ours.
	if (v->poolHandle >= 0 && !v->ours && m_bridge.DespawnRemoteVehicle)
		m_bridge.DespawnRemoteVehicle(*v);
	// Ours stays in the street, but the engine seam's record of it goes.
	if (v->poolHandle >= 0 && v->ours && m_bridge.ReleaseOwnVehicle)
		m_bridge.ReleaseOwnVehicle(*v);
	*v = RemoteVehicle{};
	Log("client: vehicle %u left", pkt.netId);
}

void Client::UpdateTrafficAllowance() {
	if (m_bridge.UpdateTrafficAllowance)
		m_bridge.UpdateTrafficAllowance();
}

void Client::OnVehicleState(const S_VehicleState &pkt) {
	// Unlike a player snapshot, a vehicle snapshot for one we've never heard
	// of isn't necessarily a race - the server may have just decided this
	// one is worth showing us now. But it carries no model, so there's
	// nothing to create from it. Ignore it and wait for the spawn, which is
	// reliable and will show up.
	RemoteVehicle *v = VehicleSlot(pkt.body.netId, /*createIfMissing=*/false);
	if (!v)
		return;

	// Who's driving is *not* taken from here, even though the packet carries
	// the sender. Enter and exit are reliable and ordered; this channel is
	// neither, so a snapshot overtaking an exit would put the driver back in
	// a car they've already stepped out of - which is exactly what the
	// ghost's -inout mode does on purpose. The enter/exit pair
	// is the only thing that decides who's in a seat, on either end.
	// A wreck takes no more orders. Its owner is dead or out of it, so
	// anything still arriving on this channel was sampled before the blast,
	// and applying it would relight a burnt-out car.
	if (v->destroyed)
		return;

	// Late, from whoever had it before it changed hands: snapshots are
	// unsequenced and the seat that moved it is not, so one can arrive after
	// the other. Taken, it would put the car back on the old driver's clock
	// and throw away the new driver's samples, twice.
	if (pkt.playerId != v->reporterPlayerId && v->reporterPlayerId != 0xFF &&
	    pkt.playerId != v->driverPlayerId && pkt.playerId != v->custodianPlayerId)
		return;

	v->last      = pkt.body;
	v->haveState = true;
	OnReportersClock(*v, pkt.playerId);
	if (pkt.playerId == VehicleHolder(*v))
		v->heardHolder = pkt.playerId;
	v->interp.Push(pkt.hdr.sendTimeMs, pkt.body.pos, pkt.body.rot,
	               MoveSpeedToMps(pkt.body.moveSpeed));

	// Arrival, not send time: the question the horn asks is how long ago we
	// last heard from the driver, and only this clock can answer it. Never 0,
	// which is the row's "no snapshot yet".
	const uint32_t now = WallClock::NowMs();
	v->lastStateAtMs   = now != 0 ? now : 1;
}

// Somebody else's car blew up.
//
// The event is what carries it, not the health field, because health is a
// number and destruction is an event - nothing in the engine watches
// m_fHealth for zero, so an observer handed a zero gets an intact-looking car
// with no health rather than a wreck. docs/protocol.md §1.11.
//
// Marked destroyed whether or not the replay lands. If the car isn't in the
// pool right now, it was still destroyed, and the one thing that must not
// happen is the roster deciding the empty slot means "respawn it".
void Client::OnVehicleBlowUp(const S_VehicleBlowUp &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.body.netId, /*createIfMissing=*/false);
	if (!v) {
		// Either a car we've never been told about or one we've already
		// forgotten. Nothing to do, and nothing to create from: the packet
		// carries no model.
		Log("client: a car blast arrived for vehicle %u, which we do not have",
		    pkt.body.netId);
		return;
	}

	if (v->destroyed)
		return;
	v->destroyed = true;

	// The wreck stays where the blast put it, so the interpolator stops here
	// too - otherwise the next frame drags it back to wherever the last
	// snapshot had it, which is a burning car sliding away from its own
	// explosion.
	v->last.pos    = pkt.body.pos;
	v->last.rot    = pkt.body.rot;
	v->last.health = 0.0f;
	v->interp      = VehicleInterpBuffer{};

	// Nobody is driving a wreck. Said before the blast, because
	// CAutomobile::BlowUpCar hands every occupant to the engine to destroy
	// and a ped taken out afterwards is a ped taken out of a car that has
	// already let go of it - the same ordering UnseatPlayer exists for.
	for (RemotePlayer &p : m_players)
		if (p.active && p.InvolvedWith(pkt.body.netId))
			UnseatPlayer(p);
	for (RemotePlayer &p : m_players)
		if (p.active && p.seatVehicleNetId == pkt.body.netId)
			p.seatVehicleNetId = INVALID_NETID;
	v->driverPlayerId = 0xFF;

	if (v->poolHandle >= 0 && m_bridge.BlowUpRemoteVehicle)
		m_bridge.BlowUpRemoteVehicle(*v, pkt.body.pos, pkt.body.rot);

	Log("client: vehicle %u blew up (player %u)", pkt.body.netId, pkt.playerId);
}

void Client::UpdateRemoteVehicles() {
	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active)
			continue;

		if (v.poolHandle < 0 && v.spawnPending && v.haveState && m_bridge.IsModelReady &&
		    m_bridge.SpawnRemoteVehicle) {
			// Ask for the model again, every pass, not just on the join
			// packet. Nothing else in the game is holding a reference to a
			// model only CoopIII's car was using, so the moment that car
			// leaves the pool the streamer is free to throw the model out -
			// and then this loop waits on an IsModelReady that will never
			// come true again and the car is simply gone for the rest of the
			// session. Reconnecting brought it back, because OnVehicleSpawn
			// is the one place that used to ask. RequestModel is idempotent
			// and costs nothing when the model is already in.
			if (m_bridge.RequestModel)
				m_bridge.RequestModel(v.modelId);
			if (m_bridge.IsModelReady(v.modelId) && m_bridge.SpawnRemoteVehicle(v)) {
				v.spawnPending = false;
				// A fresh car is undamaged whatever the row says, so the row
				// has to be put back onto it. This is also the respawn case:
				// the engine clears a car away and the roster builds a new
				// one, and without this the new one comes back pristine.
				if (v.damagePanels != 0 || v.damageDoors != 0)
					v.damagePending = true;
				Log("client: spawned vehicle %u (handle %d)", v.netId, v.poolHandle);
			}
		}

		// No snapshot is needed for the handover below: a car we claimed from
		// our own world has none until its new driver's first, a round trip
		// after the jack, and the jacker was left unseated here until then.
		if (v.poolHandle < 0)
			continue;

		// Not onto a car we are driving ourselves. The row is still here
		// because the car is still in the session, but its condition is ours
		// to report now, and writing the last driver's health, gear and
		// engine flag back onto it every snapshot would be this machine
		// arguing with itself.
		//
		// Two questions, not one, and the second is the one that was missing.
		// DrivenLocally is the session's answer - "this netId is ours" - and
		// it only becomes true when the claim comes back. LocalDrivesVehicle
		// is the engine's answer, off CVehicle::m_pDriver, and it is true the
		// moment the player is behind the wheel. A claim that is refused, or
		// dropped, or simply slow leaves a gap between the two, and the claim
		// is only ever sent once (m_vehicleClaimPending), so that gap can last
		// the whole session. Everything below writes the *previous* driver's
		// controls and velocity onto the car, every frame, which is a car the
		// player is sitting in and cannot drive.
		//
		// CorrectRemoteVehicle has asked the engine this all along, which is
		// why the transform half of the pair never had this bug and the
		// controls half did. WorldBridge::LocalDrivesVehicle.
		const bool engineSaysOurs =
		    m_bridge.LocalDrivesVehicle && m_bridge.LocalDrivesVehicle(v);

		// A third state, and it is the one the engine's answer is wrong in.
		// `surrendered` means the session has handed this car to the player
		// who jacked us while our own engine still has us behind the wheel -
		// so `engineSaysOurs` is true and stale, and taking it at face value
		// is this machine refusing every snapshot the new owner sends. The car
		// then stands in the street here while it is driven away there, which
		// is exactly what the other end of a jack looked like.
		//
		// Cleared here rather than on an event, because what ends it is the
		// engine no longer having us at the wheel - which happens when the
		// handover below lands, when the player gets out, or when the car goes
		// away under him. None of those three is a packet.
		if (v.surrendered && !engineSaysOurs) {
			v.surrendered          = false;
			v.surrenderDone        = false;
			v.surrenderHoldSinceMs = 0;
		}

		// The handover itself, once: what the end of the engine's own jack
		// does, for a jack that was not played here - takes the local player
		// out of the driver's seat and leaves the car to its new owner.
		//
		// Unless our own engine is already doing it the proper way: the jack
		// was played here too (OnJackingVehicle) and has reached our seat, or
		// is dragging us out of it. Then the handover is the drag, and doing it
		// by hand on top would cut it off and stand us beside the door. When
		// the drag lets go of the wheel `engineSaysOurs` goes false and the
		// flag above clears itself.
		if (v.surrendered && !v.surrenderDone && m_bridge.SurrenderVehicleSeat) {
			const bool dragging =
			    m_bridge.LocalBeingPulledOut && m_bridge.LocalBeingPulledOut() != PULL_NONE;
			if (KeepOutOfEnginesWay(dragging, v.surrenderHoldSinceMs, WallClock::NowMs())) {
				if (!m_saidSurrenderHeld) {
					m_saidSurrenderHeld = true;
					Log("client: player %u has vehicle %u now and our engine is "
					    "already dragging us out of it; letting the drag finish",
					    static_cast<unsigned>(v.driverPlayerId), v.netId);
				}
			} else {
				v.surrenderDone = true;
				if (m_bridge.SurrenderVehicleSeat(v))
					Log("client: handed the wheel of vehicle %u over to player %u",
					    v.netId, static_cast<unsigned>(v.driverPlayerId));
			}
		}

		if (engineSaysOurs && !DrivenLocally(v) && !v.surrendered) {
			static bool said = false;
			if (!said) {
				said = true;
				Log("client: we are at the wheel of vehicle %u and the session has "
				    "not named it ours yet, so nothing is being written onto it. "
				    "If this is the last thing said about that car, the claim never "
				    "came back and it is ours in the engine and nobody's in the "
				    "session",
				    v.netId);
			}
		}
		// One predicate, the same one the correction pass uses, so the two
		// halves of the frame cannot drift apart in what they think ownership
		// is. VehicleIsOursToDrive.
		if (VehicleIsOursToDrive(v))
			continue;

		// A car the session has handed us to settle is a car we simulate, so
		// nothing at all is written onto it - not the controls, not the
		// velocities and not the transform. It is the same rule as a car we
		// are driving, reached without a driver: an observer may animate what
		// it is watching and may not decide where it ends up, and for the
		// length of this window we are not the observer.
		//
		// Before ApplyRemoteVehicle rather than after, because the snapshot
		// it would write is the one the *previous* driver sent - the very
		// thing whose velocity, replayed for ever, is what makes a used car
		// impossible to get back into.
		if (HaveCustodyOf(v))
			continue;

		// Controls, health and flags only, and only once somebody has said
		// what they are. Transform isn't written here - anything written
		// before CGame::Process is just what local physics starts from, not
		// what actually gets drawn. See CorrectRemoteVehicles.
		if (!v.haveState)
			continue;
		if (m_bridge.ApplyRemoteVehicle)
			m_bridge.ApplyRemoteVehicle(v, v.last);

		// And then, if nobody is in the driver's seat, the controls and
		// velocities go back to rest - because there is no driver whose
		// opinion they are. A car changes hands through the reliable, ordered
		// enter/exit pair and belongs to nobody in between; driverPlayerId is
		// that fact and it is the only thing allowed to decide it (a snapshot
		// is neither reliable nor ordered - see OnVehicleState).
		//
		// Two calls rather than one branch inside ApplyRemoteVehicle, because
		// the two decide different things. Health, damage and "this car is a
		// wreck" belong to the session whoever is or is not driving - a
		// joiner has to be shown the shot-up car somebody parked. What it
		// takes a driver to mean anything is the throttle and the velocity.
		//
		// Leaving those on is what makes a used car impossible to get back
		// into: they pin m_vecMoveSpeed above CVehicle::CanPedEnterCar's
		// threshold for the rest of the session, and CPed::SeekCar answers
		// that by walking the player at the door forever. The disassembly is
		// in game/vehicle.cpp at RestRemoteVehicle.
		//
		// And not while somebody else is settling it. A custodian is an owner
		// for the length of its window (protocol.h, S_VehicleCustody), so the
		// velocities arriving for that car are a real report of a car that is
		// really moving - resting them here would leave every observer holding
		// a car that tumbles with its wheels still and its suspension asleep,
		// and would fight the correction pass for the same frames.
		if (v.driverPlayerId == 0xFF && v.custodianPlayerId == INVALID_PLAYER &&
		    m_bridge.RestRemoteVehicle)
			m_bridge.RestRemoteVehicle(v);
	}

	UpdateRemoteVehicleDamage();
}

// The other half of the two-phase spawn, for damage.
//
// Written as a reconciliation rather than as an event handler, the same shape
// as UpdateRemoteSeats and for the same reason: S_VehicleDamage is reliable
// and arrives at once, the car takes as long as its model takes to stream, and
// the engine can take the car away and give it back again underneath us. Every
// race - packet first, car first, car reaped and respawned - falls out of one
// loop instead of needing its own handler.
//
// Flying components are off here. This is the path a late joiner takes, and a
// joiner handed eight damaged cars must not be greeted by a shower of doors
// out of the object pool. docs/cardamage.md §5.2.
void Client::UpdateRemoteVehicleDamage() {
	if (!m_bridge.ApplyRemoteVehicleDamage)
		return;

	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active || !v.damagePending)
			continue;
		if (v.poolHandle < 0)
			continue;   // not here yet; the flag waits
		// A car we are driving is ours to damage. The session's record is
		// still the union of what everyone has reported and it is still
		// correct; what it must not do is reach into a car whose condition
		// this machine is now the one reporting.
		if (DrivenLocally(v))
			continue;

		VehicleDamageBody body{};
		body.netId  = v.netId;
		body.panels = v.damagePanels;
		body.doors  = v.damageDoors;
		m_bridge.ApplyRemoteVehicleDamage(v, body, /*flying=*/false);
		v.damagePending = false;
	}
}

// Who else drives or settles each session car, for the two vehicle detours.
//
// Every row with a CVehicle, and in particular the ones the correction pass
// skips - a car we drive and a car we settle. That pass used to be what wrote
// the detours' table, so for as long as we held a car the table went on
// naming whoever held it before us. Get out of a car you jacked, or one you
// took over inside someone else's settle, and our own custody car was refused
// damage and blow-up on the word of a player who no longer had it.
void Client::NoteVehicleHolders() {
	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active)
			continue;
		// A hold that has ended is over: whoever takes the car next, the same
		// player included, has to be heard from again.
		if (VehicleHolder(v) == INVALID_PLAYER)
			v.heardHolder = INVALID_PLAYER;
		if (v.poolHandle < 0 || !m_bridge.NoteVehicleHolders)
			continue;
		const VehicleHolders h = OtherVehicleHolders(v, m_localPlayerId);
		m_bridge.NoteVehicleHolders(v.netId, h.driver, h.custodian, h.weSettle,
		                            BlastFloorEnds(v, m_localPlayerId));
	}
}

// A car this machine drives or settles is this engine's to dent and to crash.
// The correction below is the only thing that used to take the
// collision-proof bit back off a copy, and it is skipped for exactly these
// cars, so a car taken over from somebody else never dented again.
void Client::TakeVehicleBack(RemoteVehicle &vehicle) {
	if (m_bridge.TakeVehicleBack)
		m_bridge.TakeVehicleBack(vehicle);
}

void Client::CorrectRemoteVehicles() {
	if (!m_bridge.CorrectRemoteVehicle)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active || v.poolHandle < 0)
			continue;

		// The car we are driving is not a car we are watching.
		//
		// This is the line that fixes "te podes subir y todo, no se puede
		// manejar ni nada". A car that arrived in the backfill is a real car
		// in the street and a joiner can get into it; the row for it stayed
		// active, and this loop went on writing the session's last known
		// transform onto it after every single frame of physics. The engine
		// turned the wheels, the suspension worked, the engine note changed,
		// and the car never went anywhere, because the last thing before the
		// frame drew was CoopIII putting it back.
		//
		// An observer may animate what it is watching and may not decide
		// where it ends up (vehicle.h). The converse is this: a driver decides
		// where their own car ends up, and nothing else may.
		//
		// Same pair of questions as UpdateRemoteVehicles, and for the same
		// reason. game/vehicle.cpp's CorrectRemoteVehicle has always asked the
		// engine's half itself, so this is not what fixes anything here - it
		// is the two halves of the frame being made to ask the same question
		// in the same place, so that the next person to change one of them
		// changes both.
		// One predicate, so that the two halves of the frame cannot drift
		// apart in what they think ownership is - including the carjack case,
		// where the engine's answer is the stale one. VehicleIsOursToDrive.
		if (VehicleIsOursToDrive(v)) {
			TakeVehicleBack(v);
			continue;
		}

		// The car we are settling is not a car we are watching either, and
		// this is the line that actually frees a wedged one. The correction
		// is applied after physics every frame, so it is the correction and
		// not the snapshot that makes a pose permanent: a car reared up
		// against a wall is put back against the wall sixty times a second,
		// and one frame of gravity never gets to finish. Stop correcting it
		// for two seconds and the engine does the rest by itself.
		if (HaveCustodyOf(v)) {
			TakeVehicleBack(v);
			continue;
		}

		// Nobody's, and our car is shoving it. Pinned, it would not budge on
		// any screen, ours included; settled, our engine moves it and the rest
		// of the session watches it move. Corrected this frame all the same:
		// the custody is a round trip away.
		if (v.driverPlayerId == 0xFF && v.custodianPlayerId == INVALID_PLAYER &&
		    !v.destroyed && m_bridge.VehiclePushedByUs &&
		    (v.pushAskedAtMs == 0 || nowMs - v.pushAskedAtMs >= PUSH_ASK_EVERY_MS) &&
		    m_bridge.VehiclePushedByUs(v)) {
			v.pushAskedAtMs = nowMs ? nowMs : 1;
			C_VehicleHit ask;
			InitHeader(ask, nowMs);
			ask.body.netId  = v.netId;
			ask.body.weapon = VEHICLE_HIT_PUSH;
			ask.body.amount = 0.0f;
			m_net.Send(ask, CH_EVENT);
			++m_pushAsks;
			if (!m_saidPushAsk) {
				m_saidPushAsk = true;
				Log("client: our car is pushing vehicle %u, which nobody holds; "
				    "asking to settle it so it moves instead of standing like a wall",
				    v.netId);
			}
		}

		// Decided here, every frame, and carried out by the seam in the call
		// below. Every frame because the engine undoes it every frame: a
		// copy with its driver seated is STATUS_PHYSICS and the horn block
		// counts the timer down, and one without is ABANDONED and its arm
		// zeroes it. game/horn.h has the rest.
		v.hornSounding = game::ReplicaHornSounds(
		    v.last.flags, v.driverPlayerId != 0xFF, v.destroyed,
		    v.lastStateAtMs, nowMs);

		VehicleTransform at;
		if (v.interp.SampleDelayed(nowMs, at)) {
			m_bridge.CorrectRemoteVehicle(v, at);
			continue;
		}

		// No snapshots yet - a car spawned from the session backfill that
		// hasn't moved since. Hold it where the spawn put it instead of
		// letting gravity and the suspension walk it down the street.
		if (v.haveState) {
			VehicleTransform spawnedAt;
			spawnedAt.pos = v.last.pos;
			spawnedAt.rot = v.last.rot;
			m_bridge.CorrectRemoteVehicle(v, spawnedAt);
		}
	}
}

void Client::OnEnterVehicle(const S_EnterVehicle &pkt) {
	if (pkt.playerId == m_localPlayerId) {
		// Our own ride, relayed back to us. A passenger's is not a claim and
		// must not be read as one.
		//
		// m_localVehicleNetId means "we are driving this and its physics are
		// ours to report" - see the field. Written for a passenger seat, as
		// it was, the very next SendLocalVehicle read the pair "not driving,
		// but we have a car", which is its definition of having just got out:
		// it sent C_ExitVehicle for the car we had got into one tick earlier,
		// the server broadcast it, and every other machine took us straight
		// back out of the seat. On their screens the passenger was in the car
		// for about forty milliseconds - long enough for the entry animation
		// to be abandoned before the door had begun to move, and not long
		// enough for anybody to see why.
		//
		// The refusal below carries seat 0 (RefuseVehicleClaim zeroes the
		// whole body), so nothing is being skipped here.
		if (pkt.body.seat != 0) {
			if (m_pendingSeatNetId == pkt.body.netId ||
			    m_localSeatNetId == pkt.body.netId)
				return;   // ours, expected, and already written down locally
			// Anything else is our own announcement coming back after we had
			// already given the seat up - an entry that ran out of time sends
			// its retraction before the echo arrives. Said once, because a
			// second cause for this line would not be harmless.
			if (!m_saidSeatEcho) {
				m_saidSeatEcho = true;
				Log("client: the session says we are riding in vehicle %u, seat "
				    "%u, and by the time it said so we were not",
				    pkt.body.netId, pkt.body.seat);
			}
			return;
		}

		// The reply to our own claim - the only thing that tells us what
		// netId the session gave to the car we're sitting in.
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;

		// A refusal. INVALID_NETID is something the server could never have
		// meant here before, which is what lets the "no" ride the same packet
		// as the "yes". Back off rather than ask again on the next frame: the
		// two things it says no to - a vehicle table that is full, and a netId
		// it has never heard of - are both states a frame does nothing to
		// change, and sixty claims a second would only make them worse.
		if (pkt.body.netId == INVALID_NETID) {
			m_localVehicleNetId = INVALID_NETID;
			m_claimRetryAtMs    = WallClock::NowMs() + CLAIM_RETRY_MS;
			// A traffic car's promotion is refused on the same packet. Left
			// pending, it was never asked for again.
			for (RemoteAmbientCar &car : m_cars)
				car.claimPending = false;
			if (!m_saidClaimRefused) {
				m_saidClaimRefused = true;
				Log("client: the session refused to name the car we are "
				    "driving, and said why in its own log. Asking again in "
				    "%u ms - until it says yes this car is ours in the engine "
				    "and nobody's in the session",
				    CLAIM_RETRY_MS);
			}
			return;
		}

		m_localVehicleNetId = pkt.body.netId;
		ForgetRejoin();
		Log("client: our vehicle is net %u", pkt.body.netId);
		// Only the driver's seat. A passenger's ride is somebody else's car
		// and it already has a row of its own.
		if (pkt.body.seat == 0)
			AdoptOurClaimedVehicle(pkt);
		return;
	}

	if (pkt.playerId >= MAX_PLAYERS)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	if (pkt.body.seat == 0)
		if (RemoteVehicle *v = VehicleSlot(pkt.body.netId, false)) {
			if (v->netId == m_rejoinAdoptedNetId)
				LetGoOfRejoinedCar(*v, pkt.playerId);
			v->driverPlayerId = pkt.playerId;
			// A driver ends a custody, here and on the server, and no packet
			// is spent saying so. "The driver, or the custodian when there is
			// no driver" is one rule with a precedence in it, not two records
			// that have to be kept in step - so a seat being taken is already
			// the whole statement, and a client that inferred anything else
			// would be holding two owners for one car.
			v->custodianPlayerId = INVALID_PLAYER;
			v->restFrames        = 0;
			v->settleReported    = false;
			// And this is where the losing end of a carjack finds out. The
			// engine here knows nothing about it - the whole jack happened in
			// the other player's process - so the packet is the only witness.
			NoteVehicleDriverChanged(*v, pkt.playerId);
		}

	// Recorded, not acted on. Either end of the pair might still be
	// streaming in - this event is reliable and arrives all at once, while
	// the ped and the car each take as long as their model does to load.
	// UpdateRemoteSeats carries it out on the first frame both exist.
	p.seatVehicleNetId = pkt.body.netId;
	p.seatIndex        = pkt.body.seat;
	// The claim is the confirmation the intent was waiting for, so the intent
	// is done - whether the entry it started finished on this machine or not.
	// Left standing it would expire a few seconds from now and take a seat
	// the session has since confirmed with it.
	p.ClearEnterIntent();
	// A fresh statement about where this player is sitting earns one fresh
	// attempt at the door-opening version of getting there. Without this
	// reset a single timed-out entry would spend the player's remaining
	// session warping, and with the reset anywhere else an entry that cannot
	// finish is retried forever.
	p.seatAnimSpent    = false;

	// The jack byte is still never set and never read. By the time this claim
	// goes out the jack is over, so an animation started from it would be a
	// whole jack late; the jack travels at its start instead, as
	// S_JackingVehicle (OnJackingVehicle), and this packet only moves the seat.
	(void)pkt.body.jack;
}

// Somebody has started getting into a car. Not "is in" - has started.
//
// This is the packet version 21 gave a passenger seat and this file said a
// driver's entry could not have. It can; what could not move was the *claim*,
// which names the car and hands over ownership and which §2.8.3 needs at the
// end. The two turned out to be separable, so they are separate: this decides
// nothing and confirms nothing, and every trace of it expires on its own.
//
// Two things ride it and both are needed. The first is *when*: an observer
// cannot animate an entry it is told about after the entry is over, and the
// only thing that ever opens that car's door on this screen is a replica
// entry of our own, played frame by frame by the entering ped's animation.
// The second is *which door*, which is not the seat: CPed::SeekCar walks a
// driver to the nearest door and the engine shuffles him across the front
// seats inside the car, so somebody who presses the enter key on the
// passenger side goes in through the right-hand door and ends up at the
// wheel. Told only "seat 0", this machine opened the driver's door and
// CPed::EnterCar's line-up dragged the replica round the car to it - which is
// exactly what the player reported seeing.
//
// The walk itself does not travel and does not need to. It is already on the
// wire, as position, at 25 Hz, and ApplyRemotePose stops writing that
// position the moment the replica's own entry starts (§1.14.5). So the ped
// walks up on the pose stream and the door opens on this one packet.
void Client::OnEnteringVehicle(const S_EnteringVehicle &pkt) {
	if (pkt.playerId == m_localPlayerId || pkt.playerId >= MAX_PLAYERS)
		return;   // our own entry is being played by our own engine

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	// A seat with no door of its own is one the animated path refuses anyway
	// (game/ped.cpp, DoorForSeat), and a door byte out of range is either a
	// build that speaks a later wire or a packet worth ignoring. Dropped
	// rather than clamped: a clamped door is a door this machine chose, and
	// choosing it is the bug.
	if (pkt.body.door > 3 || pkt.body.seat > 3)
		return;

	p.enterIntentNetId     = pkt.body.netId;
	p.enterIntentSeat      = pkt.body.seat;
	p.enterIntentDoor      = pkt.body.door;
	p.enterIntentExpiresMs = WallClock::NowMs() + ENTER_INTENT_TTL_MS;
	p.enterIntentJack      = false;
	// A fresh statement about an entry earns a fresh animated attempt, the
	// same way a fresh S_EnterVehicle does and for the same reason: without
	// it one timed-out entry spends the rest of the session warping.
	p.seatAnimSpent        = false;

	if (!m_saidEnterIntent) {
		m_saidEnterIntent = true;
		Log("client: %s is getting into vehicle %u, seat %u, through the door of "
		    "seat %u. Nothing is decided by this - the claim at the end of their "
		    "entry is still what says they got in",
		    p.nick.c_str(), pkt.body.netId, pkt.body.seat, pkt.body.door);
	}
}

// Somebody has started pulling somebody out of a car. protocol.h,
// C_JackingVehicle.
//
// Recorded as an intent like the one above, and flagged, and that flag is
// what lets UpdateRemoteSeats go where an ordinary intent may not: into a seat
// somebody is sitting in, and into a traffic car. It still decides nothing
// here. Our engine plays the jack on the replica and drags out whoever it
// finds in the seat - which, when that is us, is our own engine taking our own
// player out, the only machine that may.
void Client::OnJackingVehicle(const S_JackingVehicle &pkt) {
	if (pkt.playerId == m_localPlayerId || pkt.playerId >= MAX_PLAYERS)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	// Always for the wheel, through one of the four doors. Anything else is a
	// packet this build does not understand.
	if (pkt.body.door > 3 || pkt.body.seat != 0)
		return;

	p.enterIntentNetId     = pkt.body.netId;
	p.enterIntentSeat      = 0;
	p.enterIntentDoor      = pkt.body.door;
	p.enterIntentExpiresMs = WallClock::NowMs() + ENTER_INTENT_TTL_MS;
	p.enterIntentJack      = true;
	p.seatAnimSpent        = false;

	if (!m_saidJackIntent) {
		m_saidJackIntent = true;
		Log("client: %s is pulling somebody out of vehicle %u through the door of "
		    "seat %u; playing the jack here, and whoever sits in that seat on this "
		    "screen gets dragged out by our own engine",
		    p.nick.c_str(), pkt.body.netId, pkt.body.door);
	}
}

// The traffic half of a car's name. A replica first, then our own hosted
// traffic, which only the population seam can name.
int32_t Client::TrafficCarHandle(uint16_t netId) {
	if (netId == INVALID_NETID)
		return -1;
	if (const RemoteAmbientCar *car = AmbientCarByNetId(netId))
		return car->destroyed ? -1 : car->poolHandle;
	return m_bridge.HostedCarHandle ? m_bridge.HostedCarHandle(netId) : -1;
}

uint16_t Client::TrafficCarNetId(int32_t handle) {
	if (handle < 0)
		return INVALID_NETID;
	for (const RemoteAmbientCar &car : m_cars)
		if (car.active && !car.destroyed && car.poolHandle == handle)
			return car.netId;
	return m_bridge.HostedCarNetId ? m_bridge.HostedCarNetId(handle) : INVALID_NETID;
}

// And the sending half: tell the session the moment our own entry starts.
//
// Sent once per entry rather than every tick, because an entry lasts about a
// second and this runs 25 times inside one. The triple is what identifies it:
// the same player walking to the same door of the same car twice in a row is
// two entries, and the first one ended when SampleLocalCarEntry went false.
//
// Only for a car the session already names. A car nobody has claimed exists
// on this machine and nowhere else - the claim at the end of the entry is
// what introduces it - so there is nothing on any other screen to open a door
// on, and an intent naming INVALID_NETID would be a claim by another name.
// That is a real gap and it is the right one to have: the entry an observer
// most needs to see is somebody getting into a car it can already see.
void Client::SendLocalEntering() {
	if (!m_bridge.SampleLocalCarEntry)
		return;

	LocalCarEntry entry;
	if (!m_bridge.SampleLocalCarEntry(entry)) {
		m_enteringHandle = -1;   // not entering anything; arm the next one
		m_enteringJack   = false;
		return;
	}

	if (entry.vehicleHandle == m_enteringHandle && entry.seat == m_enteringSeat &&
	    entry.door == m_enteringDoor && entry.jack == m_enteringJack)
		return;   // already said, and it has not changed

	m_enteringHandle = entry.vehicleHandle;
	m_enteringSeat   = entry.seat;
	m_enteringDoor   = entry.door;
	m_enteringJack   = entry.jack;

	// A jack may be of a traffic car as well: taking one off its driver is
	// how a player gets a traffic car at all, and it only becomes a session
	// car at the claim.
	uint16_t netId = INVALID_NETID;
	if (const RemoteVehicle *v = VehicleByPoolHandle(entry.vehicleHandle))
		netId = v->netId;
	else if (entry.jack)
		netId = TrafficCarNetId(entry.vehicleHandle);
	m_enteringNetId = netId;
	if (netId == INVALID_NETID)
		return;

	if (entry.jack) {
		C_JackingVehicle out;
		InitHeader(out, WallClock::NowMs());
		out.body.netId = netId;
		out.body.seat  = 0;
		out.body.door  = entry.door;
		m_net.Send(out, CH_EVENT);
		if (!m_saidJackSent) {
			m_saidJackSent = true;
			Log("client: telling the session we are pulling somebody out of vehicle "
			    "%u through the door of seat %u",
			    netId, entry.door);
		}
		return;
	}

	C_EnteringVehicle out;
	InitHeader(out, WallClock::NowMs());
	out.body.netId = netId;
	out.body.seat  = entry.seat;
	out.body.door  = entry.door;
	m_net.Send(out, CH_EVENT);

	if (!m_saidEnterSent) {
		m_saidEnterSent = true;
		Log("client: telling the session we are getting into vehicle %u, seat %u, "
		    "through the door of seat %u",
		    netId, entry.seat, entry.door);
	}
}

// See the declaration in client.h for why this exists at all.
void Client::AdoptOurClaimedVehicle(const S_EnterVehicle &pkt) {
	// Without the handle there is no way to say *which* CVehicle this netId
	// names, and a row with no pool handle would be treated as a car waiting
	// to be spawned - i.e. CoopIII would build a replica of the car the
	// player is already sitting in. Say nothing and do nothing; the warning
	// for a build with that callback missing is in SendLocalVehicle.
	if (!m_bridge.SampleLocalVehicleHandle)
		return;
	const int32_t handle = m_bridge.SampleLocalVehicleHandle();
	if (handle < 0)
		return;

	RemoteVehicle *v = VehicleSlot(pkt.body.netId, /*createIfMissing=*/true);
	if (!v)
		return;

	// Re-entering a car the session already told us about (a late joiner's
	// case) finds the existing row, and that row already has everything -
	// including a pool handle for a replica CoopIII built. Do not overwrite
	// that handle with ours: they are the same CVehicle, and the row's is
	// the one every other part of the client has been using.
	const bool fresh = v->poolHandle < 0;
	if (fresh) {
		v->poolHandle = handle;
		v->ours       = true;
	}
	v->spawnPending   = false;
	v->driverPlayerId = m_localPlayerId;
	// A driver ends a custody, ours or somebody else's. The server cleared
	// its record when it took the claim, and the S_EnterVehicle for another
	// player's claim clears ours in OnEnterVehicle - but this is the branch
	// our own claim takes, and without it a car we got into inside somebody
	// else's settle kept naming them as its custodian.
	v->custodianPlayerId = INVALID_PLAYER;
	v->restFrames        = 0;
	v->settleReported    = false;
	// Ours again, whatever it was before. A car we lost to a jack and then
	// took back is the same row, and a stale `surrendered` on it would have
	// this machine watching a car it is driving.
	v->surrendered    = false;
	v->surrenderDone  = false;

	// A re-claim by netId carries nothing but the netId and the seat
	// (SendLocalVehicle zeroes the body on purpose), so only a body that
	// actually described a car is allowed to write identity.
	if (pkt.body.modelId != 0) {
		v->modelId = pkt.body.modelId;
		v->colour1 = pkt.body.colour1;
		v->colour2 = pkt.body.colour2;
		v->extra1  = pkt.body.extra1;
		v->extra2  = pkt.body.extra2;
	}

	// And the engine seam's own record of it, which the roster row does not
	// give it: the table the damage and blow-up detours read. Without it,
	// once somebody else drives this car our engine goes on denting and
	// wrecking its copy by itself. Every time, not only when fresh: a car that
	// already has a row keeps it, but the row may still name whoever drove it
	// before us, and this is the moment the session says nobody else does.
	if (v->poolHandle >= 0 && m_bridge.AdoptClaimedVehicle)
		m_bridge.AdoptClaimedVehicle(*v);

	if (fresh)
		Log("client: vehicle %u is the car we are sitting in (handle %d); the "
		    "roster keeps it so getting out and back in is the same car, not a "
		    "second one", v->netId, handle);
}

void Client::OnExitVehicle(const S_ExitVehicle &pkt) {
	if (pkt.playerId == m_localPlayerId) {
		// Logged, and it is not housekeeping. Every exit in this system was
		// silent on both ends, which is why the 2026-09-22 session's logs
		// read as "one player recorded in two cars at once" when what had
		// actually happened was an exit nobody wrote down.
		// For the car it names. The echo of an exit can land after we have
		// got into the next car and been told its name, and taken as ours it
		// dropped that name: the next send claimed the car again under a
		// second netId.
		if (pkt.netId != m_localVehicleNetId && m_localVehicleNetId != INVALID_NETID)
			return;
		Log("client: we got out of vehicle %u", m_localVehicleNetId);
		m_localVehicleNetId   = INVALID_NETID;
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;
		return;
	}
	if (RemoteVehicle *v = VehicleSlot(pkt.netId, false))
		if (v->driverPlayerId == pkt.playerId) {
			v->driverPlayerId = 0xFF;
			// And it is nobody's again, so a car that was taken off us and then
			// parked is a car we may claim. Only reachable if the handover did
			// not land - a build with SurrenderVehicleSeat missing - but
			// leaving the flag standing there would be a car the player is
			// sitting in that this machine refuses to claim for the rest of the
			// session, which is the shape of bug the flag exists to remove.
			v->surrendered   = false;
			v->surrenderDone = false;
		}

	if (pkt.playerId < MAX_PLAYERS) {
		m_players[pkt.playerId].seatVehicleNetId = INVALID_NETID;
		// And any entry they had started is over too. An exit is the
		// retraction a failed entry sends (§1.14.4), so an intent that
		// outlived it would put the ped straight back in the doorway.
		m_players[pkt.playerId].ClearEnterIntent();
		Log("client: %s got out of vehicle %u",
		    m_players[pkt.playerId].nick.c_str(), pkt.netId);
	}
}

bool Client::HaveCustodyOf(const RemoteVehicle &vehicle) const {
	return vehicle.custodianPlayerId != INVALID_PLAYER &&
	       vehicle.custodianPlayerId == m_localPlayerId;
}

// Who is simulating a car nobody is driving. protocol.h, S_VehicleCustody.
//
// The whole of the client's half of the arbitration is this one assignment.
// A machine is the custodian when the server has said so and at no other
// time: there is no test of distance, no "we were the last driver so it must
// be us", nothing a second machine could also conclude. That is deliberate
// and it is the same discipline the driver's seat is held to - the two bugs
// protocol 22 fixed were both a client working out an ownership for itself
// and being right from where it stood.
//
// Arrives on the reliable ordered channel immediately after the
// S_ExitVehicle that created the vacancy, so a client never holds a driver
// and a custodian for one car at the same instant.
void Client::OnVehicleCustody(const S_VehicleCustody &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/false);
	if (!v)
		return;

	const bool hadIt = HaveCustodyOf(*v);
	v->custodianPlayerId = pkt.playerId;

	if (!HaveCustodyOf(*v)) {
		// Handed back, or handed to somebody else. Either way this machine
		// goes back to correcting it, and the settle state has to be reset
		// rather than left - a later custody of the same car must not start
		// with ten frames of rest already banked from the last one.
		v->restFrames     = 0;
		v->settleEndsAtMs = 0;
		v->settleReported = false;
		v->holdUntilMs    = 0;
		v->burnSinceMs    = 0;
		v->sinkSinceMs    = 0;
		if (hadIt)
			Log("client: we have handed vehicle %u back to the session; every "
			    "machine holds it where it stands now", pkt.netId);
		return;
	}

	v->restFrames     = 0;
	v->settleReported = false;
	v->settleEndsAtMs = WallClock::NowMs() + VEHICLE_SETTLE_MS;
	v->holdUntilMs    = 0;
	v->burnSinceMs    = 0;
	v->sinkSinceMs    = 0;

	// A fresh high-water mark for the dents, every time. Whatever the last
	// custody of this car left in it may be above what the car wears now - a
	// respray since then is the case that would bite - and a mark above the
	// car turns every lighter dent into "nothing new" for the whole settle.
	v->settleDamagePanels = 0;
	v->settleDamageDoors  = 0;

	// The buffer belongs to whoever was driving and it is over. Left in
	// place, the correction pass would have a pose to hand back for the few
	// frames before this machine's first report goes out, and it would put
	// the car back where its last driver was rather than where it is falling
	// - which is the same mistake SendLocalVehicle's claim path clears it for.
	v->interp.Clear();

	Log("client: the session has asked us to settle vehicle %u - nobody is "
	    "driving it and our engine is the one that finishes what it was doing",
	    pkt.netId);
}

// A traffic car has stopped being traffic. protocol.h, S_CarPromoted.
//
// Nothing is created and nothing is destroyed. The CVehicle every machine
// already has stays exactly where it is and only the bookkeeping moves: the
// ambient row is retired and a vehicle row takes over the same netId and the
// same pool handle.
//
// Which matters most on the machine that was hosting it, because there the
// object is one its own engine made. A promotion that despawned and respawned
// would have that machine delete one of the player's own traffic cars and
// build a replica of it - for a car that is very possibly right in front of
// them, and quite possibly one they are about to be hit by.
void Client::OnCarPromoted(const S_CarPromoted &pkt) {
	bool weHostedIt = pkt.wasOwnerPlayerId == m_localPlayerId;

	int32_t  handle     = -1;
	uint32_t dentPanels = 0;
	uint16_t dentDoors  = 0;
	if (RemoteAmbientCar *car = AmbientCarByNetId(pkt.netId)) {
		// A row filed under us is a replica all the same: one we were handed
		// while at its wheel, which OnAmbientAdopt keeps as it is for this. It
		// becomes a copy like any other replica, not one of our engine's own
		// cars, or it would stay locked for good.
		if (car->ownerPlayerId == m_localPlayerId)
			weHostedIt = false;
		// Anybody the ambient roster had sitting in it comes out first, and
		// the ordering is the one OnCarDespawn keeps for the same reason: a
		// seat left standing against a netId that now means a different kind
		// of car is a ped warped into the wrong thing.
		for (RemoteAmbientPed &ped : m_peds) {
			if (!ped.active)
				continue;
			// Except a driver our engine is still dragging out of it, for the
			// player whose jack is this promotion: the car stays, so the seat
			// loop can wait for the drag and take him out afterwards.
			const bool dragging = ped.seatedVehicleNetId == pkt.netId &&
			                      m_bridge.AmbientBeingPulledOut &&
			                      m_bridge.AmbientBeingPulledOut(ped) != PULL_NONE;
			if (ped.seatedVehicleNetId == pkt.netId && !dragging) {
				if (m_bridge.UnseatAmbientPed)
					m_bridge.UnseatAmbientPed(ped);
				ped.seatedVehicleNetId = INVALID_NETID;
			}
			if (ped.seatVehicleNetId == pkt.netId)
				ped.seatVehicleNetId = INVALID_NETID;
		}

		handle      = car->poolHandle;
		dentPanels  = car->damagePanels;
		dentDoors   = car->damageDoors;
		*car        = RemoteAmbientCar{};
	}
	// The machine whose engine made the car has no replica row for it: its
	// own traffic is population.cpp's. Without this it built a second car
	// under the same netId and went on driving the first as traffic.
	if (handle < 0 && weHostedIt && m_bridge.HostedCarHandle)
		handle = m_bridge.HostedCarHandle(pkt.netId);

	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/true);
	if (!v) {
		Log("client: the session promoted traffic car %u and the vehicle "
		    "roster is full, so it is about to stop being drawn", pkt.netId);
		return;
	}

	v->modelId = pkt.body.modelId;
	v->colour1 = pkt.body.colour1;
	v->colour2 = pkt.body.colour2;
	v->extra1  = pkt.body.extra1;
	v->extra2  = pkt.body.extra2;

	v->last.netId  = pkt.netId;
	v->last.pos    = pkt.body.pos;
	v->last.rot    = pkt.body.rot;
	v->last.health = 1000.0f;   // VehicleStateBody::health, 1000 = full
	v->last.flags  = 0;
	v->haveState   = true;

	v->poolHandle = handle;
	// Only a machine that had no replica of this car has anything to build,
	// and it builds it the ordinary way. The point of the promotion is that
	// nobody else does.
	v->spawnPending = handle < 0;
	// Its dents come with it. A replica already wears them; a car still to be
	// built puts them on after the spawn, the way the row's own would.
	MergeDamage(v->damagePanels, v->damageDoors, dentPanels, dentDoors);
	if (handle < 0 && (v->damagePanels != 0 || v->damageDoors != 0))
		v->damagePending = true;
	if (v->spawnPending && m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.body.modelId);

	// The one machine for which this is not CoopIII's object. `ours` is the
	// flag that keeps DespawnRemoteVehicle away from it - see the field.
	v->ours = weHostedIt && handle >= 0;

	if (handle >= 0 && m_bridge.AdoptPromotedCar)
		m_bridge.AdoptPromotedCar(*v, weHostedIt);

	Log("client: traffic car %u is a session car now - player %u took the "
	    "wheel of it, it was player %u's, and %s",
	    pkt.netId, static_cast<unsigned>(pkt.driverPlayerId),
	    static_cast<unsigned>(pkt.wasOwnerPlayerId),
	    handle >= 0 ? (weHostedIt ? "it is our own engine's car"
	                              : "we keep the replica we already had")
	                : "we have to build it");
}

// Is anybody in that seat of that car, as far as the session is concerned?
//
// Asked only of an unconfirmed entry, and asked of the roster rather than of
// the engine on purpose: the question is not "is the pool slot occupied", it
// is "has the session given this seat to somebody". A confirmed seat may
// still displace whoever holds it, because the session has already decided
// that; an intent may not, because nothing has decided anything.
bool Client::SeatIsTaken(uint16_t netId, uint8_t seat, uint8_t exceptPlayer) const {
	const RemoteVehicle *v = VehicleByNetId(netId);
	if (!v)
		return false;

	// The driver's seat has a record of its own, and it is the one the
	// handover writes - so this is also what keeps an intent out of the way
	// of a jack the server is in the middle of arbitrating.
	if (seat == 0) {
		if (v->driverPlayerId != 0xFF && v->driverPlayerId != exceptPlayer)
			return true;
		if (m_localVehicleNetId == netId)
			return true;   // we are at that wheel; the handover moves it, not this
	}

	for (const RemotePlayer &other : m_players) {
		if (!other.active || other.playerId == exceptPlayer)
			continue;
		if (other.seatIndex != seat)
			continue;
		if (other.seatedVehicleNetId == netId || other.seatVehicleNetId == netId ||
		    other.enteringVehicleNetId == netId)
			return true;
	}
	// And our own passenger seat, which has no row in the loop above.
	return seat != 0 && m_localSeatNetId == netId;
}

// Makes the engine agree with the session about who's sitting where.
//
// Written as a reconciliation loop instead of a pair of event handlers
// because every interesting case here is a race: the enter event beats the
// ped's model, or the car's; the car gets despawned while somebody's in it;
// the streamer takes a ped away and hands it back. A handler would have to
// get each of those right on its own. A loop that just drives one state
// toward another gets them all right by construction, for the price of one
// comparison per player per frame.
//
// Getting in takes about a second now, and that did not change the shape of
// this - it added a third state to the two it already drove between. The
// session says `seatVehicleNetId`; the engine says `seatedVehicleNetId`; and
// in between there is `enteringVehicleNetId`, a ped walking to a door. All
// three are just fields this loop compares, and every race the animation
// adds - the car despawning mid-entry, the player dying mid-entry, the
// session naming a different car mid-entry, the engine quietly giving up -
// falls out of the same comparison rather than needing a handler.
//
// The rule that makes it safe to try at all: **the animation is an attempt
// and the seat is not.** Every path out of an attempt that did not finish
// ends at the warp, which has never failed to put a ped in a seat except
// when there was no ped or no car. A player who teleports into a seat is one
// bad frame; a player stuck half inside a car is the rest of the session.
void Client::UpdateRemoteSeats() {
	const uint32_t now = WallClock::NowMs();

	for (RemotePlayer &p : m_players) {
		if (!p.active)
			continue;

		// No ped means nothing is seated, whatever we thought before. This
		// also covers recovery from a ped the engine took away underneath
		// us - ResolveRemote clears the handle and re-arms the spawn, and
		// the new ped gets seated again on the next pass through here.
		if (p.poolHandle < 0) {
			p.seatedVehicleNetId    = INVALID_NETID;
			p.enteringVehicleNetId  = INVALID_NETID;
			continue;
		}

		// An intent that nobody ever confirmed. S_EnteringVehicle says an
		// entry has started and nothing retracts one: a player shot halfway
		// into a car sends no packet about it, because the only packet about
		// an entry is the claim at the end and there is no claim. So it
		// expires, and everything it caused goes with it - including a
		// replica that got all the way into the seat on an entry its owner
		// never finished, which the ordinary "want says on foot" arm below
		// then takes back out.
		if (p.enterIntentNetId != INVALID_NETID && !p.HasEnterIntent(now)) {
			p.ClearEnterIntent();
			if (!m_saidEnterIntentLapsed) {
				m_saidEnterIntentLapsed = true;
				Log("client: %s started getting into a car and the session never "
				    "said they got in, so the entry is being dropped. This is "
				    "what an interrupted entry looks like from here - there is "
				    "no packet for one",
				    p.nick.c_str());
			}
		}

		// What the session asks for, reduced to what's actually possible. A
		// car that hasn't spawned yet isn't a seat, so the request just
		// stands and we try again next frame.
		uint16_t       want = p.seatVehicleNetId;
		uint8_t        seat = p.seatIndex;
		uint8_t        door = p.seatIndex;
		// ...or, until the claim arrives, what the intent asks for.
		//
		// A provisional want, and the one thing it is not allowed to reach is
		// the warp at the bottom of this function: an intent is an entry that
		// *started*, and warping somebody into a seat on the strength of one
		// would seat a player who is still standing in the road. So it drives
		// the animation and nothing else - `provisional` below is that rule.
		//
		// The door comes from here and from nowhere else, because the claim
		// does not carry one and cannot: it is sent when the entry is over,
		// and by then m_vehDoor has done its work.
		//
		// And never into a seat somebody is sitting in. BeginSeatRemotePed
		// makes room the way the warp does - two peds cannot both be the
		// driver - and making room on an *unconfirmed* intent would be this
		// machine deciding that some other player left a car, which is the
		// host-authoritative rule backwards and is the whole argument
		// §1.14.6 makes about the jack animation. A jack has a handover of
		// its own now: the server arbitrates, the loser is told first, and
		// the S_EnterVehicle that follows is what moves the seat. So an
		// intent aimed at a taken seat simply waits for it.
		//
		// Unless the intent is a jack (OnJackingVehicle). Then the taken seat
		// is the point, and what empties it is not this loop but our engine's
		// own jack, which drags out whoever it finds there - on the victim's
		// machine the victim's own ped. The seat still moves only at the claim.
		const bool provisional = want == INVALID_NETID && p.HasEnterIntent(now) &&
		                         (p.enterIntentJack ||
		                          !SeatIsTaken(p.enterIntentNetId,
		                                       p.enterIntentSeat, p.playerId));
		const bool jackWanted = provisional && p.enterIntentJack;
		if (provisional) {
			want = p.enterIntentNetId;
			seat = p.enterIntentSeat;
			door = p.enterIntentDoor;
		}

		// By pool handle as well as by row, because a jack can name traffic -
		// a replica, or a car our own engine made - which has no RemoteVehicle
		// until the claim promotes it. Only a jack gets that far without one.
		RemoteVehicle *v =
		    want != INVALID_NETID ? VehicleSlot(want, /*createIfMissing=*/false)
		                          : nullptr;
		int32_t carHandle = v ? v->poolHandle : -1;
		if (!v && jackWanted)
			carHandle = TrafficCarHandle(want);
		if (carHandle < 0)
			want = INVALID_NETID;
		// And a corpse sits in nothing. OnDeath already drops the standing
		// instruction, so this only catches a seating the session hands us
		// for somebody it also says is dead - which the server no longer
		// does, and which would be a warp into a car everybody else watched
		// them die outside of.
		if (p.dead)
			want = INVALID_NETID;

		// ---- being pulled out, on the snapshot too -----------------------
		//
		// A cop at a stopped car opens the door and drags the driver out
		// (CPed::SetBeingDraggedFromCar), and this is the usual start of an
		// arrest in a car. The owner's engine carries their ped out of the
		// door, the snapshots have the positions and the animation, and the
		// exit is only sent once it is over. So the seat stands down on the
		// snapshot and the pose stream draws the drag with the owner's own
		// animation, which ApplyRemotePose writes as soon as the ped is out
		// of the seat here. A gang member jacking a player is the same case.
		{
			const uint16_t was = p.draggedFromNetId;
			p.draggedFromNetId =
			    DragLatchFor(was, p.seatVehicleNetId, p.haveState ? p.last.pedState : 0);
			if (p.draggedFromNetId != INVALID_NETID && was == INVALID_NETID)
				Log("client: %s is being pulled out of vehicle %u; their seat "
				    "stands down until the exit arrives",
				    p.nick.c_str(), p.draggedFromNetId);
			if (p.draggedFromNetId != INVALID_NETID)
				want = INVALID_NETID;
		}

		// ---- being pulled out by a jack played here -----------------------
		//
		// Somebody's jack, running on this machine (OnJackingVehicle), has
		// reached this player's seat: our engine is dragging the replica out
		// of the door, or is about to. Until that is over this loop keeps its
		// hands off him. Taking him out by hand during the drag cuts it off
		// and teleports him; taking him out just before it leaves the jacker's
		// door-open callback an empty seat, and the jacker just climbs in.
		//
		// Once the drag has started he is latched out of that car, because the
		// session goes on naming the seat until his own exit arrives, and that
		// can be a while behind a drag this machine played first.
		if (p.Seated() && !p.Entering() && m_bridge.RemoteBeingPulledOut) {
			const uint8_t pull = m_bridge.RemoteBeingPulledOut(p);
			if (pull == PULL_DRAGGED) {
				if (p.pulledOutOfNetId != p.seatedVehicleNetId && !m_saidPulledOut) {
					m_saidPulledOut = true;
					Log("client: %s is being dragged out of vehicle %u by a jack "
					    "our engine is playing; left to the engine until the drag "
					    "is over",
					    p.nick.c_str(), p.seatedVehicleNetId);
				}
				p.pulledOutOfNetId = p.seatedVehicleNetId;
				p.pulledOutAtMs    = now;
			}
			if (KeepOutOfEnginesWay(pull != PULL_NONE, p.engineHoldSinceMs, now))
				continue;
		} else {
			p.engineHoldSinceMs = 0;
		}
		p.pulledOutOfNetId =
		    PulledOutLatchFor(p.pulledOutOfNetId, p.pulledOutAtMs, p.seatVehicleNetId, now);
		if (p.pulledOutOfNetId != INVALID_NETID && want == p.pulledOutOfNetId)
			want = INVALID_NETID;

		// ---- getting out, on the snapshot rather than the event ----------
		//
		// The reliable S_ExitVehicle is not early enough to animate from:
		// the owner sends it when their own bInVehicle goes false, and that
		// is the *last* thing their get-out animation does. Starting from it
		// would put this machine a whole animation behind a player who is
		// already running down the street.
		//
		// `pedState` has been on the wire since protocol 2 and has never
		// been read by anything. It says PED_EXIT_CAR the moment the owner's
		// engine starts the exit, which is a second earlier and exactly on
		// time. The event stays the authority over whether they are out -
		// this only decides how they leave, not that they did.
		//
		// Idempotent on purpose: the bridge call returns true without doing
		// anything for a ped already on its way out, so this can stay a
		// plain per-frame condition instead of an edge.
		if (p.Seated() && !p.Entering() && want == p.seatedVehicleNetId &&
		    p.last.pedState == WIRE_PEDSTATE_EXIT_CAR &&
		    m_bridge.BeginUnseatRemotePed)
			m_bridge.BeginUnseatRemotePed(p);

		// ---- an entry already in flight ----------------------------------
		//
		// Checked before anything else, because until it is resolved this
		// player is in neither of the other two states and the comparison
		// below would read them as on foot and start again.
		if (p.Entering()) {
			// The session changed its mind, or there is no bridge: lost.
			uint8_t progress = SEAT_LOST;
			if (p.enteringVehicleNetId == want) {
				if (p.enteringJack) {
					if (m_bridge.PollJackRemotePed)
						progress = m_bridge.PollJackRemotePed(p, carHandle);
				} else if (v && m_bridge.PollSeatRemotePed) {
					progress = m_bridge.PollSeatRemotePed(p, *v, seat);
				}
			}

			if (progress == SEAT_DONE) {
				p.seatedVehicleNetId   = want;
				p.enteringVehicleNetId = INVALID_NETID;
				// The seat the engine gave, recorded whether the claim has
				// arrived or not. It has to be: an entry begun on an intent
				// finishes before the claim does, and this is the field the
				// exit path and the pose stream both read.
				p.seatIndex            = seat;
				if (p.enteringJack)
					Log("client: %s pulled whoever was driving vehicle %u out and "
					    "got in",
					    p.nick.c_str(), want);
				else
					Log("client: %s opened the door of vehicle %u and got in "
					    "(seat %u)",
					    p.nick.c_str(), want, seat);
				p.enteringJack = false;
				continue;
			}
			if (progress == SEAT_RUNNING && now < p.enterDeadlineMs)
				continue;   // still walking to the handle

			// Refused, interrupted, or out of time. Take it off the ped -
			// which is what gives the door back to the car, and now also
			// shuts it - and fall through to the seating below, in this same
			// pass. Nothing here retries: `seatAnimSpent` is already true, so
			// what follows is the warp, unless the seat is only provisional.
			if (m_bridge.AbandonSeatRemotePed)
				m_bridge.AbandonSeatRemotePed(p);
			p.enteringVehicleNetId = INVALID_NETID;
			p.enteringJack         = false;
		}

		if (p.seatedVehicleNetId == want)
			continue;

		// Always out of the old seat first, even when the move goes straight
		// from one car into another.
		UnseatPlayer(p);

		if (want == INVALID_NETID || !m_bridge.SeatRemotePed)
			continue;

		// ---- the door, if it will open -----------------------------------
		//
		// One attempt per enter event, marked spent before it is made rather
		// than after it succeeds: a refusal and a timeout both have to count,
		// or a ped that cannot get in keeps being asked to.
		//
		// Nobody else has a seat taken away for this. The seat itself is
		// still given on the same frame if the engine says no, because the
		// bridge call returns false rather than starting something.
		//
		// A jack goes in by the jack, and only by it: an ordinary entry here
		// would evict the driver the jack is meant to pull out.
		if (!provisional && p.warpSeatUntilMs != 0 &&
		    static_cast<int32_t>(p.warpSeatUntilMs - now) > 0) {
			p.seatAnimSpent   = true;   // OnCarDespawn: in once already
			p.warpSeatUntilMs = 0;
		}
		if (!p.seatAnimSpent && jackWanted && m_bridge.BeginJackRemotePed) {
			p.seatAnimSpent = true;
			if (m_bridge.BeginJackRemotePed(p, carHandle, door)) {
				p.enteringVehicleNetId = want;
				p.enteringJack         = true;
				p.seatIndex            = seat;
				p.enterDeadlineMs      = now + m_seatAnimTimeoutMs;
				if (!m_saidJackPlayed) {
					m_saidJackPlayed = true;
					Log("client: playing %s's jack of vehicle %u through the door "
					    "of seat %u",
					    p.nick.c_str(), want, door);
				}
				continue;
			}
		} else if (!p.seatAnimSpent && !jackWanted && v && m_bridge.BeginSeatRemotePed) {
			p.seatAnimSpent = true;
			if (m_bridge.BeginSeatRemotePed(p, *v, seat, door)) {
				p.enteringVehicleNetId = want;
				p.enteringJack         = false;
				p.seatIndex            = seat;
				p.enterDeadlineMs      = now + m_seatAnimTimeoutMs;
				continue;
			}
		}

		// A seat that only an intent asked for is never taken by force.
		//
		// The warp below is the answer to "the animation did not work and the
		// session says this player is in that car". An intent says no such
		// thing - it says an entry started - and the entry it describes is
		// exactly the kind that gets interrupted. Warping on one would put a
		// player in a seat they are still walking towards, or never reached,
		// and leave them there until a packet that is never coming says
		// otherwise. So a provisional want that could not be animated is
		// simply dropped; the claim at the end of the entry, if there is one,
		// seats them the ordinary way a moment later.
		if (provisional)
			continue;

		if (m_bridge.SeatRemotePed(p, *v, seat)) {
			p.seatedVehicleNetId = want;
			Log("client: %s is now in vehicle %u (seat %u)", p.nick.c_str(), want,
			    p.seatIndex);
			continue;
		}

		// It said no. If either half vanished while it was trying, that's
		// just a "not yet" and the request stands. If both are still here,
		// the engine refused a seating that nothing about the next frame is
		// going to change, and retrying sixty times a second is how a
		// one-line bug turns into a frozen game. Drop the request; the next
		// enter event will re-state it.
		if (p.poolHandle >= 0 && v->poolHandle >= 0) {
			p.seatVehicleNetId = INVALID_NETID;
			// Said, because dropping it is permanent: the only thing that
			// re-states a seat is another S_EnterVehicle, and a player who is
			// already in the car will never send one. A remote driver
			// standing in the street beside their moving car for the rest of
			// a session starts exactly here, and the old code said nothing
			// about it.
			Log("client: gave up seating %s in vehicle %u - the engine refused "
			    "and both the ped and the car are still here, so they stay on "
			    "foot until the session says otherwise",
			    p.nick.c_str(), want);
		}
	}
}

void Client::SendLocalVehicle() {
	// Not in a car - if we just were, tell the session we got out. One
	// packet on the transition, not a stream of "still not driving".
	if (!m_bridge.SampleLocalVehicle || !m_bridge.SampleLocalVehicleIdentity)
		return;

	VehicleIdentity id{};
	const bool      driving   = m_bridge.SampleLocalVehicleIdentity(id);
	const bool      rejoining = WaitingForRejoin();

	if (!driving) {
		if (m_localVehicleNetId != INVALID_NETID) {
			C_ExitVehicle out;
			InitHeader(out, WallClock::NowMs());
			out.netId = m_localVehicleNetId;
			m_net.Send(out, CH_EVENT);
			// The one line that was missing from every log this feature has
			// ever produced. An exit is what ends a car's ownership, and
			// until now it left no trace on either end - so a session where
			// somebody stepped out and got back in read exactly like a
			// session where they never got out at all.
			Log("client: telling the session we got out of vehicle %u",
			    m_localVehicleNetId);
			// The row stays. It is still a car in the session and, if it is
			// ours, it is still the car standing in the street next to us -
			// which is how getting back in claims the same netId instead of
			// registering a second one. See AdoptOurClaimedVehicle.
			if (RemoteVehicle *ours = VehicleSlot(m_localVehicleNetId, false))
				if (ours->driverPlayerId == m_localPlayerId)
					ours->driverPlayerId = 0xFF;
			m_localVehicleNetId = INVALID_NETID;
		}
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;
		return;
	}

	// Driving something the session hasn't named yet. Claim it once and
	// wait - the claim is reliable so it'll get there, and resending it
	// every frame would just register the same car over and over.
	if (m_localVehicleNetId == INVALID_NETID) {
		if (m_vehicleClaimPending)
			return;

		// Refused recently. OnEnterVehicle says why this waits rather than
		// asking again on the very next frame.
		if (WallClock::NowMs() < m_claimRetryAtMs)
			return;

		// First: is this one of the session's cars rather than one of ours?
		//
		// It can only ever be for somebody who was not here when it was
		// claimed - a late joiner - because for everyone else the car in the
		// street is a car their own engine made. That asymmetry is why this
		// went unnoticed: with two clients started together it never happens,
		// and the first person to restart their game hits it immediately.
		//
		// Claiming by the netId the session already has is the whole fix.
		// Sent with INVALID_NETID instead, the server has no way to know it
		// is the same car and allocates a second number for it: everybody
		// else spawns a duplicate on top of the original, and this machine
		// ends up driving one netId while still observing the other, which is
		// the same physical CVehicle. See CorrectRemoteVehicles.
		if (RemoteVehicle *ours = ObservedVehicleWeAreDriving()) {
			// A car the session has just taken off us is not a car to claim.
			// Our engine still has us at its wheel and will go on saying so
			// until the handover lands, so without this the very next send
			// tick claims it straight back: the player who jacked us loses it,
			// we lose it again on their next claim, and the two machines trade
			// the car at the claim rate. A handover the loser can undo is not a
			// handover.
			//
			// Returning rather than falling through matters as much as the test
			// does. Below is the arm that introduces an *unknown* car, and this
			// car is not unknown - it is the row we are standing in. Reaching
			// it would register the same CVehicle under a second netId, which
			// is the duplicate every machine then holds two cars for.
			if (ours->surrendered)
				return;
			// Taken back from the backfill a moment ago. The seats behind it
			// may yet say somebody else has had it since, and claimed now it
			// would be taken off them.
			if (rejoining && ours->netId == m_rejoinAdoptedNetId)
				return;

			C_EnterVehicle out;
			InitHeader(out, WallClock::NowMs());
			// Zeroed rather than left alone: the identity half of this body
			// is what introduces an unknown car, the server ignores it once
			// the netId names one it has, and a packed struct off the stack
			// would otherwise put whatever was there on the wire.
			out.body       = EnterVehicleBody{};
			out.body.netId = ours->netId;
			out.body.seat  = 0;
			out.body.jack  = 0;
			m_net.Send(out, CH_EVENT);
			m_vehicleClaimPending = true;

			// Its history is somebody else's and it is over. Left in place,
			// the buffer would still have a pose to hand back the moment we
			// stepped out again, and the car we just parked would jump to
			// wherever its previous driver left it. Our own reports go in
			// from here on - see the tail of this function.
			ours->interp.Clear();
			Log("client: got into the session's own vehicle %u (model %u); taking it "
			    "over rather than registering it twice", ours->netId, ours->modelId);
			return;
		}

		// Or it is a replica of somebody else's *traffic*, which is a claim
		// of its own and not this one.
		//
		// This test has to be here and it has to be before the arm below.
		// ObservedVehicleWeAreDriving looks in the vehicle roster and an
		// ambient replica is not in it, so without this the car falls through
		// to "introduce an unknown car" and gets a second netId - the same
		// duplicate §5.8.1 case 2 describes, reached from the other roster.
		// Every observer would then hold two cars for one CVehicle, one
		// following the driver and one frozen.
		//
		// ClaimDrivenAmbientCars is what asks for it, under the number the
		// session already has. protocol.h, S_CarPromoted.
		if (AmbientCarWeAreDriving())
			return;

		// The car we were in, before the backfill has had the chance to hand
		// it back under its old number.
		if (rejoining && m_rejoinNetId != INVALID_NETID && m_bridge.SampleLocalVehicleHandle &&
		    m_bridge.SampleLocalVehicleHandle() == m_rejoinHandle)
			return;

		// No match, and no way to look for one. Say so, once, and only when
		// there is actually one of the session's cars standing around for us
		// to have got into - a build with that callback left null registers
		// the car twice, and the symptom is a long way from the cause.
		if (!m_bridge.SampleLocalVehicleHandle && !m_warnedNoVehicleHandle) {
			for (const RemoteVehicle &v : m_vehicles) {
				if (!v.active || v.poolHandle < 0)
					continue;
				m_warnedNoVehicleHandle = true;
				Log("client: WorldBridge::SampleLocalVehicleHandle is not set, so we "
				    "cannot tell whether this is one of the session's own cars - if it "
				    "is, it is about to be registered a second time and will not drive");
				break;
			}
		}

		C_EnterVehicle out;
		InitHeader(out, WallClock::NowMs());
		out.body.netId   = INVALID_NETID;
		out.body.seat    = 0;
		out.body.jack    = 0;
		out.body.modelId = id.modelId;
		out.body.colour1 = id.colour1;
		out.body.colour2 = id.colour2;
		// The extras go out with the colours and for the same reason: the
		// engine rolls them per machine, so without them every observer
		// builds a car with different bits on it. docs/protocol.md §1.12.
		out.body.extra1  = id.extra1;
		out.body.extra2  = id.extra2;
		out.body.pos     = id.pos;
		out.body.rot     = id.rot;
		m_net.Send(out, CH_EVENT);
		m_vehicleClaimPending = true;
		Log("client: claiming the car we just got into (model %u, extras %d/%d)",
		    id.modelId, static_cast<int>(id.extra1), static_cast<int>(id.extra2));
		return;
	}

	VehicleStateBody body{};
	if (!m_bridge.SampleLocalVehicle(body))
		return;

	body.netId = m_localVehicleNetId;

	C_VehicleState pkt;
	InitHeader(pkt, WallClock::NowMs());
	pkt.body = body;
	m_net.Send(pkt, CH_SNAPSHOT);

	// If this is one of the session's cars that we have taken over, our own
	// reports go into its buffer too. Nothing reads them while we are
	// driving - CorrectRemoteVehicles skips a car we drive - but the moment
	// we step out they are the history that holds the car where we parked it,
	// which is the same place every other machine in the session has it. Skip
	// this and a car handed back to observation has an empty buffer and
	// drifts on local physics alone, on this screen and no other.
	if (RemoteVehicle *ours = VehicleSlot(m_localVehicleNetId, /*createIfMissing=*/false)) {
		OnReportersClock(*ours, m_localPlayerId);
		ours->interp.Push(pkt.hdr.sendTimeMs, body.pos, body.rot,
		                  MoveSpeedToMps(body.moveSpeed));
	}
}

// The cars nobody is driving that this machine has been asked to settle.
// protocol.h, S_VehicleCustody.
//
// Three things happen here and they are deliberately the only three: report
// where the car is, decide whether it has stopped, and say so once when it
// has. Nothing decides that this machine *should* be the custodian, and
// nothing keeps it after the session has said otherwise.
//
// The end condition is a run and not an instant, because the engine's own is
// (addresses.h, "when the engine itself stops believing a thing is moving").
// A car at the top of a bounce reads still for exactly one frame, and a
// single-frame test would hand it back mid-air - which is the bug this
// feature exists to remove, reached by being impatient about it.
void Client::SendCustodyVehicles() {
	if (!m_bridge.SampleObservedVehicle)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active)
			continue;
		if (!HaveCustodyOf(v) || v.settleReported)
			continue;

		// Nothing to settle. A car the engine has taken out of the pool, one
		// that blew up under us, or one the roster has no object for is a
		// custody that can only ever run to its timeout otherwise - and while
		// it ran, every observer would be waiting for reports that are never
		// coming instead of resting and pinning the car. Ended at once, which
		// is the state they should already be in.
		if (v.poolHandle < 0 || v.destroyed) {
			v.settleReported = true;
			C_VehicleSettled gone;
			InitHeader(gone, nowMs);
			gone.netId = v.netId;
			m_net.Send(gone, CH_EVENT);
			Log("client: there is nothing left of vehicle %u to settle, so the "
			    "session can have it back now", v.netId);
			continue;
		}

		VehicleStateBody body{};
		if (m_bridge.SampleObservedVehicle(v, body)) {
			body.netId = v.netId;
			C_VehicleState pkt;
			InitHeader(pkt, nowMs);
			pkt.body = body;
			m_net.Send(pkt, CH_SNAPSHOT);
			// Into our own buffer too, exactly as a car we are driving goes
			// in. The moment custody ends the correction pass starts reading
			// it again, and an empty buffer there would put the car back
			// where its last driver parked it - which is the pose we have
			// just spent two seconds getting it out of.
			OnReportersClock(v, m_localPlayerId);
			v.interp.Push(pkt.hdr.sendTimeMs, body.pos, body.rot,
			              MoveSpeedToMps(body.moveSpeed));
			v.last      = body;
			v.haveState = true;
		}

		// And its dents, on every pass, before anything below can decide the
		// settle is over. C_VehicleSettled and C_VehicleDamage share CH_EVENT,
		// so a dent from this pass reaches the server while we are still the
		// custodian and it still takes our reports. Once settleReported is set
		// the check at the top of the loop stops this, which is right: the
		// server has ended the custody by the time it reads anything after it.
		SendCustodyVehicleDamage(v, nowMs);

		// Still being pushed: kept, the way a burst of shots keeps it, so a
		// car shoved down the street is not handed back halfway and pinned.
		// And the settle starts again from here, so one shoved for longer
		// than the window still gets the whole of it to roll to a stop after
		// the last push, rather than being given up on while it rolls.
		if (m_bridge.VehiclePushedByUs && m_bridge.VehiclePushedByUs(v)) {
			v.holdUntilMs    = nowMs + CUSTODY_HIT_HOLD_MS;
			v.settleEndsAtMs = nowMs + VEHICLE_SETTLE_MS;
		}

		const bool atRest = m_bridge.VehicleAtRest && m_bridge.VehicleAtRest(v);
		if (atRest) {
			if (v.restFrames < VEHICLE_REST_FRAMES)
				++v.restFrames;
		} else {
			// One moving frame puts it back to zero, which is what the engine
			// does to m_nStaticFrames at 0x00496172. A run that a single
			// bounce can interrupt is not a run.
			v.restFrames = 0;
		}

		// A burning car is kept until it goes up: our engine is the one running
		// its fire timer, and nobody else's is.
		const bool burning = m_bridge.VehicleBurning && m_bridge.VehicleBurning(v);
		if (!burning)
			v.burnSinceMs = 0;
		else if (v.burnSinceMs == 0)
			v.burnSinceMs = nowMs != 0 ? nowMs : 1;
		if (burning && !m_saidKeptBurningCar) {
			m_saidKeptBurningCar = true;
			Log("client: vehicle %u is on fire while we settle it, so we keep it "
			    "until it goes up - our engine runs its fire timer and every other "
			    "machine holds theirs", v.netId);
		}

		// Going down in water, the same way: until it lies on the bottom, and
		// no longer than CUSTODY_SINK_CAP_MS from the first frame it was seen
		// going down in this custody. Not restarted when it stops for a frame,
		// so a car that bobs in and out of the test cannot hold on for ever.
		const bool sinking = m_bridge.VehicleSinking && m_bridge.VehicleSinking(v);
		if (sinking && v.sinkSinceMs == 0)
			v.sinkSinceMs = nowMs != 0 ? nowMs : 1;
		if (sinking && nowMs - v.sinkSinceMs < CUSTODY_SINK_CAP_MS)
			v.holdUntilMs = nowMs + CUSTODY_HIT_HOLD_MS;

		const bool settled = v.restFrames >= VEHICLE_REST_FRAMES;
		if (!CustodyMayEnd(settled, burning, nowMs, v.settleEndsAtMs, v.holdUntilMs,
		                   v.burnSinceMs))
			continue;

		// Said once. The row keeps custodianPlayerId until the server's own
		// S_VehicleCustody comes back and clears it, so that this machine and
		// everybody else stop correcting the car on the same packet rather
		// than at whatever moment each of them decided.
		v.settleReported = true;
		C_VehicleSettled done;
		InitHeader(done, nowMs);
		done.netId = v.netId;
		m_net.Send(done, CH_EVENT);
		Log("client: vehicle %u has %s - telling the session it is finished",
		    v.netId,
		    burning   ? "burned too long without going up, so we are giving it up"
		    : settled ? "come to rest"
		              : "not settled inside the window, so we are giving it up");
	}
}

// A car rolling down a slope after its driver bailed out dents on one engine,
// the custodian's, and every other machine holds it pinned and
// collision-proof. So this is the only place those dents can come from. Same
// packet, same rule and same arbitration as SendLocalVehicleDamage: absolute
// state, sent only when it grew past what we have already said, merged as a
// maximum at the server. Session::MayReportVehicle already takes it from the
// custodian when nobody is driving.
void Client::SendCustodyVehicleDamage(RemoteVehicle &v, uint32_t nowMs) {
	if (!m_bridge.SampleObservedVehicleDamage)
		return;

	VehicleDamageBody body{};
	if (!m_bridge.SampleObservedVehicleDamage(v, body))
		return;   // not a car, or a wreck: FuckCarCompletely said it everywhere

	if (!DamageGrew(v.settleDamagePanels, v.settleDamageDoors, body.panels, body.doors))
		return;

	MergeDamage(v.settleDamagePanels, v.settleDamageDoors, body.panels, body.doors);

	C_VehicleDamage out;
	InitHeader(out, nowMs);
	out.body        = VehicleDamageBody{};
	out.body.netId  = v.netId;
	out.body.panels = v.settleDamagePanels;
	out.body.doors  = v.settleDamageDoors;
	m_net.Send(out, CH_EVENT);
	++m_damageReportsSent;

	if (!m_saidSettleDamageSent) {
		m_saidSettleDamageSent = true;
		Log("client: vehicle %u took a dent while we were settling it (panels "
		    "%08X doors %04X); telling the session",
		    v.netId, static_cast<unsigned>(out.body.panels),
		    static_cast<unsigned>(out.body.doors));
	}
}

// The local player has taken the wheel of a traffic car somebody else's
// engine made. protocol.h, S_CarPromoted.
//
// The claim is the one C_EnterVehicle has always been, sent with the netId the
// session already has for the car - which is the whole reason this needed no
// new client-to-server packet. netIds are one space (Session::AllocNetId), so
// a number that names an AmbientCar can never also name a Vehicle, and the
// server can tell which kind of claim it is holding by looking.
//
// Only the driver's seat. A passenger in somebody else's traffic changes
// nothing about who simulates it, and promoting a car because somebody got
// into the back of it would take it off the machine that is still steering it.
RemoteAmbientCar *Client::AmbientCarWeAreDriving() {
	if (!m_bridge.LocalDrivesAmbientCar)
		return nullptr;
	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle < 0 || car.destroyed)
			continue;
		if (m_bridge.LocalDrivesAmbientCar(car))
			return &car;
	}
	return nullptr;
}

void Client::ClaimDrivenAmbientCars() {
	if (!m_bridge.LocalDrivesAmbientCar)
		return;

	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle < 0 || car.destroyed)
			continue;
		if (!m_bridge.LocalDrivesAmbientCar(car)) {
			// Got out again before the round trip landed, or was never in it.
			// The pending flag is dropped so a later entry claims afresh.
			car.claimPending = false;
			continue;
		}
		if (car.claimPending || WallClock::NowMs() < m_claimRetryAtMs)
			continue;

		C_EnterVehicle out;
		InitHeader(out, WallClock::NowMs());
		// Zeroed for the reason the session claim zeroes it: the identity
		// half of this body introduces an unknown car, the server ignores it
		// for a netId it already has, and a packed struct off the stack would
		// otherwise put whatever was there on the wire.
		out.body       = EnterVehicleBody{};
		out.body.netId = car.netId;
		out.body.seat  = 0;
		out.body.jack  = 0;
		m_net.Send(out, CH_EVENT);
		car.claimPending = true;
		Log("client: we are at the wheel of ambient car %u, which player %u's "
		    "engine made - asking the session to make it a session car",
		    car.netId, static_cast<unsigned>(car.ownerPlayerId));
	}
}

// ---- what shape our car is in (docs/cardamage.md) --------------------------
//
// Sampled at the snapshot rate because the car is being read anyway, and sent
// only when something actually got worse - which for most of a session is
// never. A car is undamaged for minutes and then takes a dent, so a field in
// the 25 Hz body would be 8% of the vehicle budget spent on a constant, and
// it would still say nothing about a car nobody is driving. §3.4.
void Client::SendLocalVehicleDamage() {
	if (!m_bridge.SampleLocalVehicleDamage)
		return;

	VehicleDamageBody body{};
	if (!m_bridge.SampleLocalVehicleDamage(body)) {
		// On foot, a passenger, or a wreck. The baseline is dropped rather
		// than kept: the next car we get into is a different car and would
		// otherwise be compared against this one's dents and never reported.
		m_haveSentDamage = false;
		return;
	}

	if (m_localVehicleNetId == INVALID_NETID)
		return;   // the claim is still in flight; there is nothing to name it

	// Reset the baseline on a change of car, not just on the way out, because
	// stepping straight from one car into another never passes through the
	// branch above.
	if (!m_haveSentDamage) {
		m_sentDamagePanels = 0;
		m_sentDamageDoors  = 0;
		m_haveSentDamage   = true;
		// ...and fall through, so a car that is already dented when we get
		// into it is reported once rather than kept to ourselves until the
		// next scrape.
	}

	if (!DamageGrew(m_sentDamagePanels, m_sentDamageDoors, body.panels, body.doors))
		return;

	MergeDamage(m_sentDamagePanels, m_sentDamageDoors, body.panels, body.doors);

	C_VehicleDamage out;
	InitHeader(out, WallClock::NowMs());
	out.body        = body;
	out.body.netId  = m_localVehicleNetId;
	out.body.panels = m_sentDamagePanels;
	out.body.doors  = m_sentDamageDoors;
	m_net.Send(out, CH_EVENT);
	++m_damageReportsSent;

	// M4's lesson: a feature that does nothing has to say which of the three
	// links it stopped at. This is the first one.
	if (!m_saidDamageSent) {
		m_saidDamageSent = true;
		Log("client: our car %u has taken its first dent the session does not "
		    "know about (panels %08X doors %04X); telling it",
		    m_localVehicleNetId, static_cast<unsigned>(out.body.panels),
		    static_cast<unsigned>(out.body.doors));
	}
}

// Somebody else's car got worse.
//
// Merged rather than assigned, on both ends, and that is the whole of the
// arbitration: every ladder in CDamageManager climbs and none of them
// descends, so componentwise maximum is what the engine would have produced if
// one machine had simulated every collision. Duplicates and reordering cannot
// change the answer, which is why this needs no sequence number and no
// entitlement check of its own - the server has already done the only one that
// means anything.
void Client::OnVehicleDamage(const S_VehicleDamage &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.body.netId, /*createIfMissing=*/false);
	if (!v || !v->active) {
		// Traffic, from the machine hosting it. netIds are one space, so the
		// number cannot mean a session car as well.
		if (RemoteAmbientCar *car = AmbientCarByNetId(pkt.body.netId)) {
			OnAmbientCarDamage(*car, pkt);
			return;
		}
		// Never heard of it. Not created from: the packet carries no model,
		// and a dent is not a reason to invent a car. The backfill is what
		// covers a joiner.
		return;
	}

	// A wreck is finished. Its damage came from FuckCarCompletely on every
	// machine and there is nothing this could add.
	if (v->destroyed)
		return;

	// A repair, not a report. Somebody's car came out of a Pay'n'Spray and the
	// respray packet that came just before this one has already run
	// CAutomobile::Fix on our copy. All that is left is to stop believing in
	// the dents, so the row does not put them back the next time this car is
	// spawned and a later dent below them is not read as "nothing new".
	//
	// Nothing is applied to the car here on purpose: ApplyRemoteVehicleDamage
	// never lowers anything - it writes a status and calls the engine's
	// applier per component, and there is no component-level undo - so the
	// only thing that can clean a car is the Fix the respray already did.
	if (IsDamageReset(pkt.body.panels)) {
		v->damagePanels  = 0;
		v->damageDoors   = 0;
		v->damagePending = false;
		return;
	}

	const uint32_t wasPanels = v->damagePanels;
	const uint16_t wasDoors  = v->damageDoors;
	MergeDamage(v->damagePanels, v->damageDoors, pkt.body.panels, pkt.body.doors);
	if (v->damagePanels == wasPanels && v->damageDoors == wasDoors)
		return;   // said nothing new

	if (!m_saidDamageReceived) {
		m_saidDamageReceived = true;
		Log("client: took a car's damage off the wire for vehicle %u from "
		    "player %u (panels %08X doors %04X)",
		    pkt.body.netId, pkt.playerId,
		    static_cast<unsigned>(v->damagePanels),
		    static_cast<unsigned>(v->damageDoors));
	}

	// Applied straight away when the car is here, with the parts flying,
	// because this is a change happening in front of you rather than a
	// restore. Otherwise the row holds it and UpdateRemoteVehicleDamage
	// applies it after the spawn, with them off.
	if (v->poolHandle >= 0 && !DrivenLocally(*v) && m_bridge.ApplyRemoteVehicleDamage) {
		VehicleDamageBody body{};
		body.netId  = v->netId;
		body.panels = v->damagePanels;
		body.doors  = v->damageDoors;
		m_bridge.ApplyRemoteVehicleDamage(*v, body, /*flying=*/true);
		v->damagePending = false;
	} else {
		v->damagePending = true;
	}
}

// ---- ambient population (docs/population.md §3 step 2) ---------------------
//
// One direction, one kind of entity, no generation changes. A pedestrian this
// machine's engine created gets announced, the server names it, and every
// other machine builds a matching one that is written to and never decides
// anything.
//
// **This is not shippable on its own and the design says so** (§3 step 3).
// Every machine is still generating its own crowd, and now every machine also
// holds everybody else's, so two players make roughly twice the pedestrians.
// The fix is the counter rewriting, and it is deliberately not here.

RemoteAmbientPed *Client::AmbientPedByNetId(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (RemoteAmbientPed &ped : m_peds)
		if (ped.active && ped.netId == netId)
			return &ped;
	return nullptr;
}

void Client::ClearAmbientPeds() {
	uint32_t dropped = 0;
	for (RemoteAmbientPed &ped : m_peds) {
		if (!ped.active)
			continue;
		// Out of the seat before the ped is destroyed, the same ordering
		// ClearRoster keeps for players and for the same reason: a car left
		// holding a reference to a ped that no longer exists writes a nil
		// into a pool slot somebody else owns by then.
		if (ped.Seated() && m_bridge.UnseatAmbientPed)
			m_bridge.UnseatAmbientPed(ped);
		ped.seatedVehicleNetId = INVALID_NETID;
		if (ped.poolHandle >= 0 && m_bridge.DespawnAmbientReplica)
			m_bridge.DespawnAmbientReplica(ped);
		ped = RemoteAmbientPed{};
		++dropped;
	}
	m_pedClaimCount = 0;
	m_warnedPedRosterFull = false;
	m_warnedPedClaimsFull = false;
	if (dropped != 0)
		Log("client: dropped %u ambient ped replica(s)", dropped);
}

void Client::OnPedSpawn(const S_PedSpawn &pkt) {
	// Ours coming back with a name on it. Recognised by both halves, because
	// tempIds are per-machine and two clients allocate the same numbers: the
	// owner has to match as well or this machine would claim somebody else's
	// ped the moment their counters lined up with ours.
	if (pkt.tempId != 0 && pkt.ownerPlayerId == m_localPlayerId) {
		bool wasOurs = false;
		for (uint32_t i = 0; i < m_pedClaimCount; ++i) {
			if (m_pedClaims[i] != pkt.tempId)
				continue;
			m_pedClaims[i] = m_pedClaims[--m_pedClaimCount];
			wasOurs = true;
			break;
		}
		if (!wasOurs) {
			// A name for a ped we never claimed. Says the session and this
			// machine disagree about who owns what, which is worth a line.
			Log("client: the session named ped %u under temp id %u, which we "
			    "never claimed", pkt.netId, pkt.tempId);
			return;
		}
		if (!m_bridge.NameLocalAmbientPed)
			return;
		if (m_bridge.NameLocalAmbientPed(pkt.tempId, pkt.netId))
			return;

		// The engine reaped it while the round trip was in flight. Normal, and
		// the only correct answer is to tell the session at once - otherwise
		// every other machine keeps a replica of a pedestrian that no longer
		// exists anywhere.
		C_PedDespawn gone;
		InitHeader(gone, WallClock::NowMs());
		gone.netId = pkt.netId;
		m_net.Send(gone, CH_EVENT);
		return;
	}

	// Somebody else's. Never our own - a machine does not observe what it
	// hosts, it *is* the host, and its engine already has the real ped.
	if (pkt.ownerPlayerId == m_localPlayerId)
		return;
	if (AmbientPedByNetId(pkt.netId))
		return;   // already known; a backfill and a live spawn can overlap

	RemoteAmbientPed *slot = nullptr;
	for (RemoteAmbientPed &ped : m_peds)
		if (!ped.active) {
			slot = &ped;
			break;
		}
	if (!slot) {
		if (!m_warnedPedRosterFull) {
			m_warnedPedRosterFull = true;
			Log("client: the ambient ped roster is full (%zu); ignoring further "
			    "spawns (and not saying so again)", MAX_REMOTE_PEDS);
		}
		return;
	}

	*slot = RemoteAmbientPed{};
	slot->active        = true;
	slot->netId         = pkt.netId;
	slot->ownerPlayerId = pkt.ownerPlayerId;
	slot->body          = pkt.body;
	slot->spawnPending  = true;
	// Seed the held pose from the spawn, so a ped nobody is streaming yet -
	// or a backfilled one whose owner is a hundred metres away and is
	// spending its twelve slots elsewhere - is held where the session says
	// it was born rather than at whatever a default-constructed Pose means.
	slot->last.pos      = pkt.body.pos;
	slot->last.heading  = pkt.body.heading;
	if (m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.body.modelId);
}

void Client::OnPedDespawn(const S_PedDespawn &pkt) {
	RemoteAmbientPed *ped = AmbientPedByNetId(pkt.netId);
	if (!ped)
		return;
	// Emptied before it is destroyed, same ordering as ClearAmbientPeds.
	if (ped->Seated() && m_bridge.UnseatAmbientPed)
		m_bridge.UnseatAmbientPed(*ped);
	ped->seatedVehicleNetId = INVALID_NETID;
	if (ped->poolHandle >= 0 && m_bridge.DespawnAmbientReplica)
		m_bridge.DespawnAmbientReplica(*ped);
	*ped = RemoteAmbientPed{};
}

void Client::OnPedBodyPart(const S_PedBodyPart &pkt) {
	// The server already refused anything but the five limbs; checked again
	// here because the node is an index the engine trusts, and a client does
	// not get to assume which server it is talking to.
	if (!IsRemovableBodyPart(pkt.body.node))
		return;
	RemoteAmbientPed *ped = AmbientPedByNetId(pkt.body.netId);
	if (!ped) {
		// A player's own limb, from their own machine. Ours never comes back:
		// the server does not send it to us, and our own engine took it off.
		for (RemotePlayer &p : m_players)
			if (p.active && p.playerId != m_localPlayerId && p.netId == pkt.body.netId) {
				if (p.poolHandle >= 0 && m_bridge.RemovePlayerBodyPart)
					m_bridge.RemovePlayerBodyPart(p, pkt.body.node, pkt.body.direction);
				return;
			}
		return;
	}
	// Not ours to show if we never built it: a replica still waiting on its
	// model, or a ped this machine hosts itself, whose limb our own engine
	// took off already. The server does not send that one back, and this is
	// the guard for when it does.
	if (ped->poolHandle < 0 || !m_bridge.RemoveAmbientBodyPart)
		return;
	m_bridge.RemoveAmbientBodyPart(*ped, pkt.body.node, pkt.body.direction);
}

// Somebody else's pedestrian died. Recorded, not carried out.
//
// Same shape as Client::OnDeath for a player, and the same bug avoided: the
// replica may not exist yet. A live death can overtake its own ped's spawn
// while the model is still streaming, and a backfilled corpse's S_PedDeath
// arrives in the same burst as the S_PedSpawn that asks for it - so "if
// there is a ped, kill it" would lose the death in the ordinary case rather
// than the rare one, and leave a pedestrian walking around a session that
// knows he is dead. UpdateRemoteAmbientPeds does the killing, on the first
// frame there is something to kill.
//
// A ped we have no row for is not an error: it is one this machine hosts
// itself, whose death our own engine decided and the server does not send
// back, or one whose spawn we never saw.
void Client::OnPedDeath(const S_PedDeath &pkt) {
	RemoteAmbientPed *ped = AmbientPedByNetId(pkt.body.netId);
	if (!ped)
		return;

	ped->dead         = true;
	ped->deathAnimId  = pkt.body.animId;
	ped->deathApplied = false;

	// And stop asking for the seat. Left standing, UpdateAmbientPedSeats
	// would put the corpse back behind the wheel on the very next frame -
	// the same instruction Client::OnDeath cancels for a player, and it
	// matters more here because CPed::SetDie's PED_DRIVING arm hands a
	// non-player ped to the engine to destroy.
	ped->seatVehicleNetId = INVALID_NETID;
}

// Somebody else's player shot one of *our* pedestrians.
//
// The only inbound ambient packet that is carried out on the spot rather than
// recorded on a row, and the difference is not an inconsistency. A death, a
// limb and a seat are standing facts about a pedestrian: they outlive any
// particular replica of him, so they are written down and reconciled every
// frame until they take. A hit is not a fact about the pedestrian at all - it
// is a thing that happened to the one who was standing there when the trigger
// was pulled. If he is gone, there is nothing for it to be true of.
//
// There is also no row to write it on. This is the one ambient packet about a
// ped this machine *hosts* rather than replicates, so `m_peds` has nothing in
// it for this netId: the engine's own pedestrian is game/population.cpp's, and
// resolving the name is behind the bridge for the same reason every other
// engine lookup is.
void Client::OnPedDamage(const S_PedDamage &pkt) {
	if (!m_bridge.ApplyRemotePedDamage) {
		// Said once, and it is the line that tells "this build cannot" apart
		// from "this build would not". A whole session was lost to a chain that
		// did nothing and said nothing about which link stopped it.
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: a hit on one of our pedestrians arrived and this build has "
			    "no bridge to apply it with, so nobody will ever be able to hurt "
			    "our NPCs");
		}
		return;
	}

	// Blame, where it resolves. A null attacker still hurts the pedestrian: the
	// shooter's ped may not have streamed in here, and the shot happened either
	// way. Deliberately *not* folded into m_lastAttackerNetId - that field is
	// who last hurt the local player, and a pedestrian dying has no bearing on
	// who gets credit for killing us.
	RemotePlayer *attacker = nullptr;
	if (pkt.attackerId < MAX_PLAYERS && pkt.attackerId != m_localPlayerId &&
	    m_players[pkt.attackerId].active)
		attacker = &m_players[pkt.attackerId];

	m_bridge.ApplyRemotePedDamage(attacker, pkt.body);
}

// ---- chat --------------------------------------------------------------------

void Client::OnChat(const S_Chat &pkt) {
	char text[CHAT_LEN];
	std::memcpy(text, pkt.text, sizeof text);
	text[CHAT_LEN - 1] = '\0';   // the wire field isn't guaranteed to end

	// Chat and joins share the ordered channel, so a line from somebody we
	// have not been told about is not something the server sends.
	const char *nick = "?";
	if (pkt.playerId == m_localPlayerId)
		nick = m_localNick.empty() ? "you" : m_localNick.c_str();
	else if (pkt.playerId < MAX_PLAYERS && m_players[pkt.playerId].active)
		nick = m_players[pkt.playerId].nick.c_str();

	const bool known = pkt.playerId == m_localPlayerId ||
	                   (pkt.playerId < MAX_PLAYERS && m_players[pkt.playerId].active);
	char   line[FEED_MESSAGE];
	size_t nickLen = 0;
	FormatChatLine(line, sizeof line, nick, text, &nickLen);
	m_feed.Push(FeedKind::Chat, line, WallClock::NowMs(),
	            known ? pkt.playerId : INVALID_PLAYER, nickLen);

	static bool said = false;
	if (!said) {
		said = true;
		Log("client: chat is arriving - the first line was from %s", nick);
	}
}

void Client::SendTypedChat() {
	if (!m_bridge.TakeTypedChat)
		return;
	char text[CHAT_LEN];
	if (!m_bridge.TakeTypedChat(text))
		return;
	C_Chat out;
	InitHeader(out, WallClock::NowMs());
	std::memcpy(out.text, text, sizeof out.text);
	out.text[CHAT_LEN - 1] = '\0';
	m_net.Send(out, CH_EVENT);

	static bool said = false;
	if (!said) {
		said = true;
		Log("client: sent our first line of chat");
	}
}

// ---- somebody else's pedestrians fighting (protocol.h, C_NpcShot) -----------

// A round one of their pedestrians fired. Drawn on our copy of him if we have
// one and dropped otherwise, the way a remote player's round is: a streak only
// means anything at the instant it was fired.
void Client::OnNpcShot(const S_NpcShot &pkt) {
	if (pkt.ownerPlayerId == m_localPlayerId || !m_bridge.ReplayAmbientShot)
		return;
	RemoteAmbientPed *ped = AmbientPedByNetId(pkt.pedNetId);
	if (!ped || ped->ownerPlayerId != pkt.ownerPlayerId || ped->poolHandle < 0 || ped->dead)
		return;
	m_bridge.ReplayAmbientShot(*ped, pkt.body);
}

// A hit one of their pedestrians landed on our copy on their machine, which is
// the only place it could be found. Applied here like any other hit on us.
void Client::OnNpcDamage(const S_NpcDamage &pkt) {
	if (m_localNetId == INVALID_NETID || pkt.body.victimNetId != m_localNetId) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("client: an NPC's hit arrived for net %u and we are net %u, so it is "
			    "not ours to apply", pkt.body.victimNetId, m_localNetId);
		}
		return;
	}
	if (!m_bridge.ApplyNpcDamage)
		return;

	RemoteAmbientPed *attacker = AmbientPedByNetId(pkt.attackerPedNetId);
	if (attacker && attacker->ownerPlayerId != pkt.ownerPlayerId)
		attacker = nullptr;

	// Whoever hurt us last is no longer a player, so a death from here on is
	// not theirs to be credited with.
	m_lastAttackerNetId = INVALID_NETID;
	m_lastAttackerMs    = 0;

	static bool said = false;
	if (!said) {
		said = true;
		Log("client: our first hit from somebody else's NPC arrived - pedestrian net "
		    "%u, hosted by player %u, cause %u, %.0f (%s)", pkt.attackerPedNetId,
		    pkt.ownerPlayerId, pkt.body.weapon, pkt.body.amount,
		    attacker ? "we have his replica for the blame" : "we have no replica of him");
	}
	m_bridge.ApplyNpcDamage(attacker, pkt.body);
}

void Client::UpdateRemoteAmbientPeds() {
	const bool canSpawn = m_bridge.SpawnAmbientReplica && m_bridge.IsModelReady;

	for (RemoteAmbientPed &ped : m_peds) {
		if (!ped.active)
			continue;

		// Is what we built still there? This runs before the spawn pass, the
		// death pass, the seat pass and the pose pass, so a replica the engine
		// took away is noticed by everything downstream on the same frame it
		// went rather than being written into for the rest of the session.
		//
		// It is also what makes the two "the engine took it away underneath
		// us, the spawn pass builds a new one" comments true - here and in
		// UpdateAmbientPedSeats. They were written before anything reset
		// RemoteAmbientPed::poolHandle, so nothing ever did.
		//
		// Unconditional on purpose: `canSpawn` gates the spawn block below and
		// nothing else, because a build that cannot spawn must still not keep
		// a handle to an object the engine has taken, and a ped that already
		// has a replica has to fall through to the death pass rather than
		// being skipped here.
		if (ped.poolHandle >= 0 && m_bridge.AmbientReplicaIsAlive)
			m_bridge.AmbientReplicaIsAlive(ped);

		if (canSpawn && ped.poolHandle < 0 && ped.spawnPending) {
			// Asked for every pass, not once on the packet. The streamer is
			// free to evict a model nothing else is holding, and a one-shot
			// request leaves this loop waiting on an IsModelReady that never
			// comes true again - the same trap UpdateRemoteVehicles fell
			// into.
			if (m_bridge.RequestModel)
				m_bridge.RequestModel(ped.body.modelId);
			if (!m_bridge.IsModelReady(ped.body.modelId))
				continue;
			if (m_bridge.SpawnAmbientReplica(ped)) {
				ped.spawnPending = false;
				// A ped that has just been built has not been killed,
				// whatever was done to the last copy of him. Without this a
				// replica that died, was lost and was rebuilt would come
				// back on his feet in a session that knows he is dead -
				// `dead` is the fact and `deathApplied` is only a note about
				// one particular replica.
				ped.deathApplied = false;
			}
		}

		// The death the session told us about, carried out on the first
		// frame there is a ped to carry it out on - which is this one when
		// the replica was already up, and a later one when the death
		// overtook the spawn or arrived in a joiner's backfill. Retried
		// rather than dropped, because a replica lost to a full pool and
		// rebuilt has to die again: `dead` is a standing fact about the
		// pedestrian and outlives any particular copy of him.
		if (!ped.dead || ped.deathApplied || ped.poolHandle < 0 ||
		    !m_bridge.KillAmbientReplica)
			continue;

		// Out of the seat first, through the bridge, so this machine's own
		// record of where he is sitting agrees with what the engine is about
		// to be told. KillAmbientReplica checks the ped's state and unseats
		// it again if it has to - that check is the load-bearing one, since
		// CPed::SetDie's PED_DRIVING arm would flag the replica for
		// destruction - but leaving `seatedVehicleNetId` set here would have
		// UpdateAmbientPedSeats trying to take a corpse out of a car on
		// every frame for as long as it lies there.
		if (ped.Seated() && m_bridge.UnseatAmbientPed)
			m_bridge.UnseatAmbientPed(ped);
		ped.seatedVehicleNetId = INVALID_NETID;
		ped.seatVehicleNetId   = INVALID_NETID;

		if (m_bridge.KillAmbientReplica(ped, ped.deathAnimId))
			ped.deathApplied = true;
	}
}

void Client::SendLocalAmbientPeds() {
	// Limbs before anything else. A pedestrian can lose one and be reaped in
	// the same frame - a rocket in a crowd does both - and a limb that arrives
	// after its ped's despawn finds nothing to come off. CH_EVENT is ordered,
	// so sending it first is all it takes.
	if (m_bridge.DrainAmbientBodyParts) {
		PedBodyPartBody limbs[16];
		const uint32_t n = m_bridge.DrainAmbientBodyParts(limbs, 16);
		for (uint32_t i = 0; i < n; ++i) {
			C_PedBodyPart out;
			InitHeader(out, WallClock::NowMs());
			out.body = limbs[i];
			// Our own player's, under the netId the session gave us, which
			// the server takes from us and nobody else.
			if (out.body.netId == INVALID_NETID) {
				if (m_localNetId == INVALID_NETID)
					continue;
				out.body.netId = m_localNetId;
				if (!m_saidOwnLimbSent) {
					m_saidOwnLimbSent = true;
					Log("client: our player lost a limb (node %u); telling the session "
					    "so it comes off on every screen", out.body.node);
				}
			}
			m_net.Send(out, CH_EVENT);
		}
	}

	// Then the deaths, and the three-way order limbs -> deaths -> despawns is
	// the host's own order rather than a preference.
	//
	// CPed::InflictDamage takes the limb off first and calls CPed::SetDie at
	// its tail (addresses.h, at CPed__RemoveBodyPart), so a headshot really
	// does happen in that order on the machine that decided it, and CH_EVENT
	// is ordered, so sending it in that order is all it takes to replay it.
	// The two commute on the engine side - SetDie never touches m_pFrames and
	// RemoveBodyPart never touches the ped state - so getting it wrong would
	// have been invisible, which is exactly why it is written down.
	//
	// The despawn goes last because it ends both: a death announced after its
	// ped has been taken away finds nothing to kill, and the ped pool reuses
	// a slot the instant it frees up.
	if (m_bridge.DrainAmbientPedDeaths) {
		PedDeathBody deaths[16];
		const uint32_t n = m_bridge.DrainAmbientPedDeaths(deaths, 16);
		for (uint32_t i = 0; i < n; ++i) {
			C_PedDeath out;
			InitHeader(out, WallClock::NowMs());
			out.body = deaths[i];
			m_net.Send(out, CH_EVENT);
		}
	}

	// The peds that have gone, and that order is deliberate: a slot in
	// the engine's ped pool is reused the instant it frees up, so announcing
	// a birth before the death that made room for it would have every other
	// machine holding two replicas of one pool slot.
	if (m_bridge.DrainLostAmbientPeds) {
		uint16_t gone[32];
		const uint32_t n = m_bridge.DrainLostAmbientPeds(gone, 32);
		for (uint32_t i = 0; i < n; ++i) {
			C_PedDespawn out;
			InitHeader(out, WallClock::NowMs());
			out.netId = gone[i];
			m_net.Send(out, CH_EVENT);
		}
	}

	if (!m_bridge.DrainLocalAmbientPeds)
		return;

	// A bound per frame rather than "everything waiting". The engine makes
	// pedestrians in bursts - GeneratePedsAtStartOfGame alone runs the
	// generator fifty times in one frame - and a burst of fifty reliable
	// packets is a stall on a connection that has anything else to do. What
	// is left over stays in population.cpp's queue and goes next frame.
	LocalAmbientPed born[16];
	const uint32_t n = m_bridge.DrainLocalAmbientPeds(born, 16);
	for (uint32_t i = 0; i < n; ++i) {
		if (m_pedClaimCount >= MAX_PENDING_PED_CLAIMS) {
			if (!m_warnedPedClaimsFull) {
				m_warnedPedClaimsFull = true;
				Log("client: %zu ambient ped claims are already in flight; "
				    "dropping the rest (and not saying so again)",
				    MAX_PENDING_PED_CLAIMS);
			}
			return;
		}

		m_pedClaims[m_pedClaimCount++] = born[i].tempId;

		C_PedSpawn out;
		InitHeader(out, WallClock::NowMs());
		out.tempId = born[i].tempId;
		out.body   = born[i].body;
		m_net.Send(out, CH_EVENT);
	}
}

// ---- the ambient ped stream (docs/population.md §3 step 6) ------------------
//
// Step 2 shipped a pedestrian that appears on the other screen and stays
// where it is put, and said so deliberately. What it looks like in a running
// game is a city of statues: the host's pedestrians walk around its own
// Liberty City and on every other machine they stand exactly where they were
// created, for as long as the host keeps them.
//
// This is the stream that fixes it, and it is the traffic stream with two
// differences. It is cheaper per row - a ped is slower and smaller than a
// car, so there is no velocity on the wire and no animation phase
// (protocol.h, AmbientPedState). And it carries one thing the car stream has
// no use for: which car this pedestrian is sitting in.

void Client::OnPedStates(const S_PedStates &pkt) {
	const uint8_t n = pkt.count < MAX_PED_STATES ? pkt.count : MAX_PED_STATES;
	uint32_t applied = 0;
	for (uint8_t i = 0; i < n; ++i) {
		const AmbientPedState &in = pkt.peds[i];
		RemoteAmbientPed *ped = AmbientPedByNetId(in.netId);
		if (!ped)
			continue;
		// Only from the machine that hosts it, the same second half of the
		// same rule OnCarStates applies: the server already refuses to relay
		// anybody else's, and this is the half that does not depend on the
		// server being the version that enforces it.
		if (ped->ownerPlayerId != pkt.ownerPlayerId)
			continue;

		// Velocity is deliberately zero rather than absent: the buffer takes
		// one, and a pedestrian's is not worth twelve bytes a row
		// (protocol.h). A zero makes it hold when the stream runs dry, which
		// for something moving at 1.5 m/s is the better of the two answers.
		ped->interp.Push(pkt.hdr.sendTimeMs, in.pos, in.heading, Vec3{0, 0, 0});
		ped->last.pos     = in.pos;
		ped->last.heading = in.heading;
		ped->animId       = in.animId;
		// On WallClock, not the sender's timestamp: what matters is how long
		// ago anybody vouched for the fire, here.
		const uint32_t at = WallClock::NowMs();
		ped->fireSaid    = (in.flags & AMBIENT_PED_ON_FIRE) != 0;
		ped->fireSaidMs  = at;
		ped->lastRowAtMs = at != 0 ? at : 1;

		// The standing instruction, not the act. UpdateAmbientPedSeats is
		// what carries it out, on whatever frame both halves exist.
		ped->seatVehicleNetId = in.vehicleNetId;
		ped->seatIndex        = in.seat;
		// And the same for what is in his hand. ApplyAmbientPedPoses arms him.
		ped->weapon = AmbientPedWeapon(in.flags);
		++applied;
	}

	// Said once, and it is this stream's only witness - exactly the argument
	// OnCarStates makes. A replica that is never told where it is looks
	// precisely like one that works, until you walk down the street.
	static bool said = false;
	if (!said && applied != 0) {
		said = true;
		Log("client: ambient ped states are arriving - %u of %u in the first "
		    "batch from player %u applied (and this will not be said again)",
		    applied, n, pkt.ownerPlayerId);
	}
}

void Client::ApplyAmbientPedPoses() {
	if (!m_bridge.ApplyAmbientPedState)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteAmbientPed &ped : m_peds) {
		if (!ped.active || ped.poolHandle < 0)
			continue;

		// Before the seat test, on purpose: a replica that sits down while
		// burning has to have its fire put out (pedanim.h, PlanRemoteFire),
		// and a seated one is skipped below.
		if (m_bridge.ApplyAmbientPedFire)
			m_bridge.ApplyAmbientPedFire(ped);

		// A seated ped is positioned by CWorld::Process from its car's
		// matrix, every frame (docs/protocol.md §1.13.2). Writing a position
		// over the top of that is what would drag him half out of the seat
		// for exactly the part of the frame physics and collision look at -
		// the same rule, and the same reason, as ApplyRemotePose's early
		// return for a seated player.
		if (ped.Seated())
			continue;

		// The weapon his host has him holding, on a change. Not while seated:
		// the warp into a seat is the engine's and so is what it does to the
		// hand, and a gun that goes back in on the way out is ours to put
		// there again (UnseatAmbientPed clears appliedWeapon).
		if (ped.weapon != ped.appliedWeapon && !ped.dead && m_bridge.ArmAmbientReplica &&
		    m_bridge.ArmAmbientReplica(ped, ped.weapon))
			ped.appliedWeapon = ped.weapon;

		// A ped waiting for its turn in its owner's batch (protocol.h,
		// MAX_PED_STATES) does not empty this buffer; its rows just pause.
		// SampleDelayed then holds it on the newest one, because OnPedStates
		// pushes a zero velocity and there is nothing to extrapolate along -
		// a person standing still, the far band of §2.1's rate-by-distance.
		// The traffic version of this needed SampleDelayedHeld; this one gets
		// the same answer for free.
		Pose at;
		if (!ped.interp.SampleDelayed(nowMs, at)) {
			// Nothing in the buffer, which only happens before the first
			// row: hold it where the spawn put it.
			at = ped.last;
		}
		m_bridge.ApplyAmbientPedState(ped, at);
	}
}

// The traffic driver, reconciled rather than handled.
//
// `seatVehicleNetId` is what the owner keeps saying; `seatedVehicleNetId` is
// what this machine has actually done. Driving one toward the other on every
// frame is why none of the races here need a handler of their own: the ped
// replica and the car replica are created independently and each waits on its
// own model, either can be lost to a full pool and rebuilt, and the car can be
// taken away underneath the ped. UpdateRemoteSeats is the same loop over
// players and the reasoning is written out there.
void Client::UpdateAmbientPedSeats() {
	for (RemoteAmbientPed &ped : m_peds) {
		if (!ped.active)
			continue;

		// No ped means nothing is seated, whatever we thought. Covers the
		// replica the engine took away underneath us: the spawn pass runs
		// WorldBridge::AmbientReplicaIsAlive before this loop, so by the time
		// we get here either the replica has already been rebuilt - and this
		// seats it in the same frame - or its model has been evicted and the
		// handle is clear, which is this arm. Either way the instruction to
		// sit in a car stands and nothing here retires it. That ordering is
		// the whole of the recovery, and this comment used to claim it while
		// nothing anywhere reset the handle.
		if (ped.poolHandle < 0) {
			ped.seatedVehicleNetId = INVALID_NETID;
			continue;
		}

		// What the session asks for, reduced to what is possible. A car whose
		// replica does not exist yet is not a seat, so the request just
		// stands.
		uint16_t          want = ped.seatVehicleNetId;
		RemoteAmbientCar *car  = want != INVALID_NETID ? AmbientCarByNetId(want)
		                                               : nullptr;
		if (!car || car->poolHandle < 0 || car->ownerPlayerId != ped.ownerPlayerId)
			want = INVALID_NETID;

		// A player's jack, played here, has reached this driver: our engine
		// is dragging the replica out, or is about to. Hands off until it is
		// done, and then keep him out of that car while his host's rows catch
		// up - its own engine is dragging the real driver out too, a moment
		// before or after ours. UpdateRemoteSeats does the same for a player.
		if (ped.Seated() && m_bridge.AmbientBeingPulledOut) {
			const uint8_t pull = m_bridge.AmbientBeingPulledOut(ped);
			if (pull == PULL_DRAGGED) {
				ped.pulledOutOfNetId = ped.seatedVehicleNetId;
				ped.pulledOutAtMs    = WallClock::NowMs();
			}
			if (KeepOutOfEnginesWay(pull != PULL_NONE, ped.engineHoldSinceMs,
			                        WallClock::NowMs()))
				continue;
		} else {
			ped.engineHoldSinceMs = 0;
		}
		ped.pulledOutOfNetId = PulledOutLatchFor(ped.pulledOutOfNetId, ped.pulledOutAtMs,
		                                         ped.seatVehicleNetId, WallClock::NowMs());
		if (ped.pulledOutOfNetId != INVALID_NETID && want == ped.pulledOutOfNetId)
			want = INVALID_NETID;

		if (ped.seatedVehicleNetId == want)
			continue;

		// Always out of the old seat first, even when the move goes straight
		// from one car into another.
		if (ped.Seated() && m_bridge.UnseatAmbientPed)
			m_bridge.UnseatAmbientPed(ped);
		ped.seatedVehicleNetId = INVALID_NETID;

		if (want == INVALID_NETID || !m_bridge.SeatAmbientPed)
			continue;

		if (m_bridge.SeatAmbientPed(ped, *car, ped.seatIndex)) {
			ped.seatedVehicleNetId = want;
			// The interpolation buffer belongs to a ped on foot. Kept across
			// a seating it would hand ApplyAmbientPedPoses a stale timeline
			// the moment he gets out again, and drag him back to wherever the
			// car picked him up.
			ped.interp.Clear();
			continue;
		}

		// It said no. If either half vanished while it was trying, that is a
		// "not yet" and the request stands. If both are still here the engine
		// refused a seating nothing about the next frame will change, and
		// retrying sixty times a second is how a one-line bug becomes a
		// frozen game - so drop it. The owner restates it on its next batch,
		// which is the advantage of a stream over an event.
		if (ped.poolHandle >= 0 && car->poolHandle >= 0)
			ped.seatVehicleNetId = INVALID_NETID;
	}
}

// The observer's half of docs/population.md §2.1: of the replicas we hold,
// how many have heard from their host lately. A replica its host's batches
// never get round to stands where it was last heard of, and on this screen
// that looks exactly like one that is working until somebody walks up to it.
// Corpses and wrecks are left out, since nothing moves them.
void Client::ReportAmbientFreshness() {
	constexpr uint32_t REPORT_MS = 5000;
	constexpr uint32_t FRESH_MS  = 3000;
	const uint32_t nowMs = WallClock::NowMs();
	if (nowMs - m_lastFreshnessReportMs < REPORT_MS)
		return;
	m_lastFreshnessReportMs = nowMs;

	uint32_t peds = 0, pedsFresh = 0, cars = 0, carsFresh = 0;
	for (const RemoteAmbientPed &ped : m_peds) {
		if (!ped.active || ped.poolHandle < 0 || ped.dead)
			continue;
		++peds;
		if (ped.lastRowAtMs != 0 && nowMs - ped.lastRowAtMs <= FRESH_MS)
			++pedsFresh;
	}
	for (const RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle < 0 || car.destroyed)
			continue;
		++cars;
		if (car.lastRowAtMs != 0 && nowMs - car.lastRowAtMs <= FRESH_MS)
			++carsFresh;
	}
	if (peds == 0 && cars == 0)
		return;
	Log("client: other machines' crowd here - %u of %u ped replica(s) and %u of "
	    "%u car replica(s) had a row in the last %u s", pedsFresh, peds, carsFresh,
	    cars, FRESH_MS / 1000);
}

bool Client::AmbientDriverSaid(const RemoteAmbientCar &car) const {
	for (const RemoteAmbientPed &ped : m_peds)
		if (ped.active && ped.ownerPlayerId == car.ownerPlayerId &&
		    ped.seatVehicleNetId == car.netId && ped.seatIndex == 0)
			return true;
	return false;
}

void Client::SendHostedPedStates() {
	if (!m_bridge.SampleHostedPeds)
		return;

	// 10 Hz, on a limiter of its own. The same argument as the traffic
	// stream's and the same number, arrived at differently: a car is fast
	// enough that 10 Hz is already coarse, and a pedestrian is slow enough
	// that 10 Hz is generous. What buys the ped stream its place in the
	// budget is the row being 24 bytes rather than 44, not the clock.
	// game/streampick.h counts its deadlines in these ticks.
	constexpr uint32_t PED_STATE_INTERVAL_MS = game::STREAM_TICK_MS;
	const uint32_t nowMs = WallClock::NowMs();
	if (nowMs - m_lastPedStateMs < PED_STATE_INTERVAL_MS)
		return;
	m_lastPedStateMs = nowMs;

	Vec3 viewers[MAX_PLAYERS];
	const uint32_t viewerCount = ViewerPositions(viewers, MAX_PLAYERS);

	C_PedStates out;
	InitHeader(out, nowMs);
	out.count = static_cast<uint8_t>(
	    m_bridge.SampleHostedPeds(out.peds, MAX_PED_STATES, viewers, viewerCount));
	if (out.count == 0)
		return;
	m_net.Send(out, CH_SNAPSHOT);
}

// ---- ambient traffic (docs/population.md §3 step 4) -------------------------
//
// The pedestrian pass above with the names changed, plus the one thing a
// pedestrian never needed: a traffic car is going somewhere, so the machine
// that hosts it has to keep saying where, and every observer has to put its
// replica back there after each frame of local physics.
//
// The reason that is not optional: a replica is created the CREATE_CAR way,
// which means locked, which means *the local engine will never clear it
// away*. A ped replica standing still is a person standing still. A car
// replica standing still is a permanent roadblock, and one the player can
// crash into.

RemoteAmbientCar *Client::AmbientCarByNetId(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (RemoteAmbientCar &car : m_cars)
		if (car.active && car.netId == netId)
			return &car;
	return nullptr;
}

void Client::ClearAmbientCars() {
	uint32_t dropped = 0;
	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active)
			continue;
		if (car.poolHandle >= 0 && m_bridge.DespawnAmbientCarReplica)
			m_bridge.DespawnAmbientCarReplica(car);
		car = RemoteAmbientCar{};
		++dropped;
	}
	m_carClaimCount       = 0;
	m_warnedCarRosterFull = false;
	m_warnedCarClaimsFull = false;
	if (dropped != 0)
		Log("client: dropped %u ambient car replica(s)", dropped);
}

void Client::OnCarSpawn(const S_CarSpawn &pkt) {
	// Ours, coming back with a name on it. Both halves have to match for the
	// reason the ped version spells out: tempIds are per-machine and two
	// clients allocate the same numbers.
	if (pkt.tempId != 0 && pkt.ownerPlayerId == m_localPlayerId) {
		bool wasOurs = false;
		for (uint32_t i = 0; i < m_carClaimCount; ++i) {
			if (m_carClaims[i] != pkt.tempId)
				continue;
			m_carClaims[i] = m_carClaims[--m_carClaimCount];
			wasOurs = true;
			break;
		}
		if (!wasOurs) {
			Log("client: the session named car %u under temp id %u, which we "
			    "never claimed", pkt.netId, pkt.tempId);
			return;
		}
		if (!m_bridge.NameLocalAmbientCar)
			return;
		if (m_bridge.NameLocalAmbientCar(pkt.tempId, pkt.netId))
			return;

		// Recycled while the round trip was in flight. Traffic goes through
		// this more often than pedestrians do: CCarCtrl removes a car as soon
		// as it is far enough behind the player, and cars get behind you fast.
		C_CarDespawn gone;
		InitHeader(gone, WallClock::NowMs());
		gone.netId = pkt.netId;
		m_net.Send(gone, CH_EVENT);
		return;
	}

	if (pkt.ownerPlayerId == m_localPlayerId)
		return;
	if (AmbientCarByNetId(pkt.netId))
		return;   // a backfill and a live spawn can overlap

	RemoteAmbientCar *slot = nullptr;
	for (RemoteAmbientCar &car : m_cars)
		if (!car.active) {
			slot = &car;
			break;
		}
	if (!slot) {
		if (!m_warnedCarRosterFull) {
			m_warnedCarRosterFull = true;
			Log("client: the ambient car roster is full (%zu); ignoring further "
			    "spawns (and not saying so again)", MAX_REMOTE_CARS);
		}
		return;
	}

	*slot = RemoteAmbientCar{};
	slot->active        = true;
	slot->netId         = pkt.netId;
	slot->ownerPlayerId = pkt.ownerPlayerId;
	slot->body          = pkt.body;
	slot->spawnPending  = true;
	// Seeded now rather than on the first snapshot, so the per-frame
	// correction has something to hold the car at from the frame it exists.
	// Without it a car spawned from the backfill gets however many frames of
	// gravity and suspension it takes the owner's stream to reach us, which
	// on a slope is a car rolling away downhill.
	slot->last.pos      = pkt.body.pos;
	slot->last.rot      = pkt.body.rot;
	if (m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.body.modelId);
}

void Client::OnCarDespawn(const S_CarDespawn &pkt) {
	RemoteAmbientCar *car = AmbientCarByNetId(pkt.netId);
	if (!car)
		return;

	// Anybody sitting in it gets out first, and the ordering is the same one
	// TestCarIsEmptiedBeforeItIsDestroyed pins for players: destroying a car
	// under a seated ped leaves that ped with bInVehicle set and a null
	// m_pMyVehicle, and CPed::ProcessControl follows the flag.
	//
	// The standing instruction is dropped too, not just the seat. Left in
	// place it would have UpdateAmbientPedSeats asking every frame for a car
	// the session has taken away - and if the netId were ever reused, seating
	// the ped in a different car entirely.
	for (RemoteAmbientPed &ped : m_peds) {
		if (!ped.active)
			continue;
		if (ped.seatedVehicleNetId == pkt.netId) {
			if (m_bridge.UnseatAmbientPed)
				m_bridge.UnseatAmbientPed(ped);
			ped.seatedVehicleNetId = INVALID_NETID;
		}
		if (ped.seatVehicleNetId == pkt.netId)
			ped.seatVehicleNetId = INVALID_NETID;
	}
	// And a player whose jack of it we played, sitting in it or halfway in,
	// the same way. Only a jack ever seats a player in traffic, and the usual
	// reason it goes now is that the jacker hosted it: its host's claim drops
	// the traffic car and names a new one in the same place, a packet behind.
	// They are already in on this screen, so that seating is a warp.
	for (RemotePlayer &p : m_players)
		if (p.active && p.InvolvedWith(pkt.netId)) {
			if (p.Seated())
				p.warpSeatUntilMs = WallClock::NowMs() + 2000;
			UnseatPlayer(p);
		}

	if (car->poolHandle >= 0 && m_bridge.DespawnAmbientCarReplica)
		m_bridge.DespawnAmbientCarReplica(*car);
	*car = RemoteAmbientCar{};
}

void Client::OnCarStates(const S_CarStates &pkt) {
	const uint8_t n = pkt.count < MAX_CAR_STATES ? pkt.count : MAX_CAR_STATES;
	uint32_t applied = 0;
	for (uint8_t i = 0; i < n; ++i) {
		const AmbientCarState &in = pkt.cars[i];
		RemoteAmbientCar *car = AmbientCarByNetId(in.netId);
		if (!car)
			continue;
		// Only from the machine that hosts it. The server already refuses to
		// relay anybody else's, so this is the second half of the same rule
		// rather than the only one - and it is the half that does not depend
		// on the server being the version that enforces it.
		if (car->ownerPlayerId != pkt.ownerPlayerId)
			continue;

		car->interp.Push(pkt.hdr.sendTimeMs, in.pos, in.rot, MoveSpeedToMps(in.velocity));
		car->last.pos = in.pos;
		car->last.rot = in.rot;
		// Kept as it was when the sender didn't say (an older build writes 0).
		DecodeAmbientHealth(in.health, car->health);
		// Arrival time, as the player's horn does it: the question is how
		// long ago the host last said anything about this car. Never 0.
		car->hornOnWire  = CarStateHornSet(pkt.hornMask, i);
		car->sirenOnWire = CarStateSirenSet(pkt.sirenMask, i);
		const uint32_t at = WallClock::NowMs();
		car->lastRowAtMs  = at != 0 ? at : 1;
		++applied;
	}

	// Said once, and it is the only witness this stream has.
	//
	// Everything else about ambient traffic shows up in a count: a replica
	// that was never created is a number that does not move. A replica that
	// exists and is never *told where it is* looks exactly like one that
	// works, until you walk down the street and find a car standing in it.
	// So the first batch that actually lands says so.
	static bool said = false;
	if (!said && applied != 0) {
		said = true;
		Log("client: ambient car transforms are arriving - %u of %u in the "
		    "first batch from player %u applied (and this will not be said "
		    "again)", applied, n, pkt.ownerPlayerId);
	}
}

void Client::UpdateRemoteAmbientCars() {
	if (!m_bridge.SpawnAmbientCarReplica || !m_bridge.IsModelReady)
		return;

	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle >= 0 || !car.spawnPending)
			continue;
		// The session says this one burned out. The local engine reaps a
		// wreck a minute after it dies, and CorrectAmbientCarReplica re-arms
		// the spawn whenever a replica goes missing from the pool - so
		// without this the shell would come back as a brand new car, with the
		// host still streaming a transform for the wreck it is (roadmap 5.8).
		if (car.destroyed)
			continue;

		// Asked every pass, not once on the packet - the streamer is free to
		// evict a model nothing else is holding, and a one-shot request
		// leaves this waiting on an IsModelReady that never comes true again.
		if (m_bridge.RequestModel)
			m_bridge.RequestModel(car.body.modelId);
		if (!m_bridge.IsModelReady(car.body.modelId))
			continue;
		if (!m_bridge.SpawnAmbientCarReplica(car))
			continue;
		car.spawnPending = false;

		// Arrives already dented, without a shower of parts: this is a spawn
		// or a rebuild, not a crash in front of anybody.
		if ((car.damagePanels != 0 || car.damageDoors != 0) &&
		    m_bridge.ApplyAmbientCarDamage) {
			VehicleDamageBody body{};
			body.netId  = car.netId;
			body.panels = car.damagePanels;
			body.doors  = car.damageDoors;
			m_bridge.ApplyAmbientCarDamage(car, body, /*flying=*/false);
		}
	}
}

void Client::CorrectAmbientCars() {
	if (!m_bridge.CorrectAmbientCarReplica)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle < 0)
			continue;

		// Decided here and carried out by the seam in the call below, every
		// frame, because the engine takes it back every frame (game/horn.h).
		car.hornTimer = game::TrafficHornTimer(
		    game::ReplicaTrafficHornSounds(car.hornOnWire, car.destroyed,
		                                   car.lastRowAtMs, nowMs),
		    car.hornLeft);

		// The siren, the same way: decided here, written after physics.
		// Whether somebody is at the wheel is only looked up for a car whose
		// siren is on, since nothing else reads it.
		car.sirenOn = game::ReplicaTrafficSirenOn(car.sirenOnWire, car.destroyed);
		car.driverSaid = car.sirenOn && AmbientDriverSaid(car);

		// Held, not extrapolated, when the rows stop.
		//
		// They pause all the time without the car going anywhere: its host's
		// batch holds MAX_CAR_STATES and its cars take turns at it
		// (game/streampick.h), so a car far from everybody can miss a few
		// batches in a row, and one lost packet is a missed row too. No
		// despawn, nothing said, and the buffer keeps its samples - it never
		// empties while the replica lives. With
		// plain SampleDelayed that left the car coasting along its last
		// velocity until the playback clock settled, some 220 ms past the
		// last row, and standing there: over 5 m further down the road at
		// 25 m/s, in a straight line off any bend, past where a car braking
		// for a light really stopped - and when the rows came back, dragged
		// backwards to meet them. SampleDelayedHeld stops it on the last row,
		// which is what population.md §2.1 always said this did. When the
		// car's turn comes, its rows land in the same buffer and it drives on
		// from there - the same replica, forwards.
		//
		// Not handed to the local physics either, which would keep it moving:
		// its autopilot is zeroed, so it would roll to a stop wherever this
		// machine's friction and kerbs put it, into whatever this machine has
		// parked there. Where a traffic car is belongs to its host
		// (docs/protocol.md §1.23), and a pose it sent is the only one an
		// observer has.
		VehicleTransform at;
		if (car.interp.SampleDelayedHeld(nowMs, at)) {
			m_bridge.CorrectAmbientCarReplica(car, at);
			continue;
		}

		// Nothing in the buffer, which only happens before the first row:
		// hold it at the spawn transform rather than hand it to the local
		// suspension, which walks a standing car down a hill over a couple
		// of minutes.
		m_bridge.CorrectAmbientCarReplica(car, car.last);
	}
}

void Client::SendLocalAmbientCars() {
	// Deaths before births, the same ordering and for the same reason as the
	// pedestrian pass: a vehicle pool slot is reused the instant it frees up.
	if (m_bridge.DrainLostAmbientCars) {
		uint16_t gone[16];
		const uint32_t n = m_bridge.DrainLostAmbientCars(gone, 16);
		for (uint32_t i = 0; i < n; ++i) {
			C_CarDespawn out;
			InitHeader(out, WallClock::NowMs());
			out.netId = gone[i];
			m_net.Send(out, CH_EVENT);
		}
	}

	if (!m_bridge.DrainLocalAmbientCars)
		return;

	LocalAmbientCar born[8];
	const uint32_t n = m_bridge.DrainLocalAmbientCars(born, 8);
	for (uint32_t i = 0; i < n; ++i) {
		if (m_carClaimCount >= MAX_PENDING_CAR_CLAIMS) {
			if (!m_warnedCarClaimsFull) {
				m_warnedCarClaimsFull = true;
				Log("client: %zu ambient car claims are already in flight; "
				    "dropping the rest (and not saying so again)",
				    MAX_PENDING_CAR_CLAIMS);
			}
			return;
		}

		m_carClaims[m_carClaimCount++] = born[i].tempId;

		C_CarSpawn out;
		InitHeader(out, WallClock::NowMs());
		out.tempId = born[i].tempId;
		out.body   = born[i].body;
		m_net.Send(out, CH_EVENT);
	}
}

void Client::SendHostedCarStates() {
	if (!m_bridge.SampleHostedCars)
		return;

	// 10 Hz, on a limiter of its own, deliberately not m_sendRate's 25.
	//
	// docs/population.md §2.1 is the whole argument: a dozen ambient cars at
	// the player rate is most of a session's budget on its own, and it says
	// the rate has to fall off with distance rather than be flat. So there is
	// a slower clock for everything ambient, eight cars inside each tick, and
	// the cars take turns at those eight by how near they are to the players
	// who will see them (game/streampick.h).
	constexpr uint32_t CAR_STATE_INTERVAL_MS = game::STREAM_TICK_MS;
	const uint32_t nowMs = WallClock::NowMs();
	if (nowMs - m_lastCarStateMs < CAR_STATE_INTERVAL_MS)
		return;
	m_lastCarStateMs = nowMs;

	// On the same clock: a dent is rare, and ten reads a second of the
	// cars we host is plenty to catch one.
	SendHostedCarDamage(nowMs);

	Vec3 viewers[MAX_PLAYERS];
	const uint32_t viewerCount = ViewerPositions(viewers, MAX_PLAYERS);

	C_CarStates out;
	InitHeader(out, nowMs);
	uint8_t horns = 0, sirens = 0;
	out.count = static_cast<uint8_t>(m_bridge.SampleHostedCars(
	    out.cars, MAX_CAR_STATES, viewers, viewerCount, horns, sirens));
	out.hornMask  = horns;
	out.sirenMask = sirens;
	if (out.count == 0)
		return;
	m_net.Send(out, CH_SNAPSHOT);
}

// Our own traffic's dents, on the packet a driver's go on. The server takes it
// from a car's host as it takes it from a car's driver (Session::NoteCarDamage).
void Client::SendHostedCarDamage(uint32_t nowMs) {
	if (!m_bridge.DrainHostedCarDamage)
		return;
	VehicleDamageBody dents[MAX_CAR_STATES];
	const uint32_t n = m_bridge.DrainHostedCarDamage(dents, MAX_CAR_STATES);
	for (uint32_t i = 0; i < n; ++i) {
		C_VehicleDamage out;
		InitHeader(out, nowMs);
		out.body = dents[i];
		m_net.Send(out, CH_EVENT);
		++m_damageReportsSent;
	}
}

// Somebody else's traffic car got worse, as its host's engine says. The
// session car's rules: merged as a maximum, worn straight away when the
// replica is here, and held on the row until it is when it is not.
void Client::OnAmbientCarDamage(RemoteAmbientCar &car, const S_VehicleDamage &pkt) {
	if (car.destroyed || car.ownerPlayerId == m_localPlayerId)
		return;
	if (IsDamageReset(pkt.body.panels))
		return;   // traffic is never resprayed; a session car would be

	const uint32_t wasPanels = car.damagePanels;
	const uint16_t wasDoors  = car.damageDoors;
	MergeDamage(car.damagePanels, car.damageDoors, pkt.body.panels, pkt.body.doors);
	if (car.damagePanels == wasPanels && car.damageDoors == wasDoors)
		return;

	if (!m_saidCarDamageReceived) {
		m_saidCarDamageReceived = true;
		Log("client: took our first traffic dent off the wire, for car %u from "
		    "player %u (panels %08X doors %04X)",
		    car.netId, pkt.playerId, static_cast<unsigned>(car.damagePanels),
		    static_cast<unsigned>(car.damageDoors));
	}

	if (car.poolHandle >= 0 && m_bridge.ApplyAmbientCarDamage) {
		VehicleDamageBody body{};
		body.netId  = car.netId;
		body.panels = car.damagePanels;
		body.doors  = car.damageDoors;
		m_bridge.ApplyAmbientCarDamage(car, body, /*flying=*/true);
	}
}

// ---- a leaver's crowd (protocol.h, S_AmbientAdopt) --------------------------
//
// Sent just before the S_PlayerLeave, and it only ever names things the
// leaver hosted. The server has already moved its own rows; this moves ours.
//
// Peds first, then cars. A ped we are given but cannot take is let go of here,
// and letting go of a seated replica takes it out of its seat; done before the
// car is converted, the car then sees an empty driver's seat and is parked
// rather than set cruising with nobody in it.

void Client::AdoptPedHere(RemoteAmbientPed *ped, uint16_t netId, uint32_t &taken,
                          uint32_t &released) {
	const game::AdoptPlan plan =
	    game::PlanPedAdoption(ped != nullptr, ped && ped->poolHandle >= 0,
	                          ped && ped->dead, ped ? ped->body.pedType : 0);
	if (plan == game::AdoptPlan::Convert && m_bridge.AdoptAmbientPed &&
	    m_bridge.AdoptAmbientPed(*ped)) {
		// The object is population.cpp's now and the bridge has cleared the
		// handle, so dropping the row touches nothing in the engine.
		*ped = RemoteAmbientPed{};
		++taken;
		return;
	}

	// Ours to let go of: the server takes this from the owner, and tells
	// everybody else.
	C_PedDespawn out;
	InitHeader(out, WallClock::NowMs());
	out.netId = netId;
	m_net.Send(out, CH_EVENT);
	if (ped) {
		S_PedDespawn gone{};
		InitHeader(gone, WallClock::NowMs());
		gone.netId = netId;
		OnPedDespawn(gone);
	}
	++released;
}

void Client::AdoptCarHere(RemoteAmbientCar *car, uint16_t netId, uint32_t &taken,
                          uint32_t &released) {
	const bool atWheel =
	    car && (car->claimPending ||
	            (m_bridge.LocalDrivesAmbientCar && m_bridge.LocalDrivesAmbientCar(*car)));
	const game::AdoptPlan plan = game::PlanCarAdoption(
	    car != nullptr, car && car->poolHandle >= 0, car && car->destroyed, atWheel);

	if (plan == game::AdoptPlan::KeepForClaim) {
		// The promotion our own claim asked for is on its way and will make
		// this a session car. Kept as the replica it is, with us as the owner
		// the session now says we are, so OnCarPromoted finds the handle.
		car->ownerPlayerId = m_localPlayerId;
		car->interp.Clear();
		++taken;
		Log("client: we were given traffic car %u while at its wheel; it stays as it is "
		    "until our claim on it lands", netId);
		return;
	}
	if (plan == game::AdoptPlan::Convert && m_bridge.AdoptAmbientCar &&
	    m_bridge.AdoptAmbientCar(*car)) {
		*car = RemoteAmbientCar{};
		++taken;
		return;
	}

	C_CarDespawn out;
	InitHeader(out, WallClock::NowMs());
	out.netId = netId;
	m_net.Send(out, CH_EVENT);
	if (car) {
		S_CarDespawn gone{};
		InitHeader(gone, WallClock::NowMs());
		gone.netId = netId;
		OnCarDespawn(gone);
	}
	++released;
}

void Client::OnAmbientAdopt(const S_AmbientAdopt &pkt) {
	const uint8_t n = pkt.count < MAX_ADOPT_ROWS ? pkt.count : MAX_ADOPT_ROWS;

	uint32_t pedsTaken = 0, carsTaken = 0, pedsLetGo = 0, carsLetGo = 0;
	uint32_t pedsMoved[MAX_PLAYERS] = {}, carsMoved[MAX_PLAYERS] = {};

	for (int pass = 0; pass < 2; ++pass) {
		const uint8_t kind = pass == 0 ? AMBIENT_ADOPT_PED : AMBIENT_ADOPT_CAR;
		for (uint8_t i = 0; i < n; ++i) {
			const AmbientAdoptRow &r = pkt.rows[i];
			if (r.kind != kind || r.netId == INVALID_NETID)
				continue;

			if (r.newOwnerPlayerId == m_localPlayerId) {
				if (kind == AMBIENT_ADOPT_PED)
					AdoptPedHere(AmbientPedByNetId(r.netId), r.netId, pedsTaken, pedsLetGo);
				else
					AdoptCarHere(AmbientCarByNetId(r.netId), r.netId, carsTaken, carsLetGo);
				continue;
			}
			if (r.newOwnerPlayerId >= MAX_PLAYERS)
				continue;

			// Somebody else's now. The replica stays exactly as it is; only
			// whose rows it takes changes, and the rows already buffered are
			// stamped on the leaver's clock, which the new owner does not share.
			if (kind == AMBIENT_ADOPT_PED) {
				if (RemoteAmbientPed *ped = AmbientPedByNetId(r.netId)) {
					ped->ownerPlayerId = r.newOwnerPlayerId;
					ped->interp.Clear();
					++pedsMoved[r.newOwnerPlayerId];
				}
			} else if (RemoteAmbientCar *car = AmbientCarByNetId(r.netId)) {
				car->ownerPlayerId = r.newOwnerPlayerId;
				car->interp.Clear();
				++carsMoved[r.newOwnerPlayerId];
			}
		}
	}

	const char *leaver = pkt.wasOwnerPlayerId < MAX_PLAYERS &&
	                             m_players[pkt.wasOwnerPlayerId].active
	                         ? m_players[pkt.wasOwnerPlayerId].nick.c_str()
	                         : "a player who left";
	if (pedsTaken != 0 || carsTaken != 0 || pedsLetGo != 0 || carsLetGo != 0)
		Log("client: %s left and we host %u of their pedestrian(s) and %u of their "
		    "car(s) now; %u ped(s) and %u car(s) we could not take are let go",
		    leaver, pedsTaken, carsTaken, pedsLetGo, carsLetGo);
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (pedsMoved[id] != 0 || carsMoved[id] != 0)
			Log("client: %u ped(s) and %u car(s) %s left behind are player %u's now; "
			    "our replicas stay and take his rows",
			    pedsMoved[id], carsMoved[id], leaver, static_cast<unsigned>(id));
}

} // namespace coopiii
