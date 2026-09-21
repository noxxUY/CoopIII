#include "client.h"

#include "log.h"

#include <cstring>

namespace coopiii {

bool Client::Start(const std::string &host, uint16_t port, const std::string &nick,
                   const WorldBridge &bridge) {
	m_bridge = bridge;
	ClearRoster();
	return m_net.Start(host, port, nick);
}

void Client::Stop() {
	m_net.Stop();
	ClearRoster();
}

uint8_t Client::RemoteCount() const {
	uint8_t n = 0;
	for (const RemotePlayer &p : m_players)
		if (p.active)
			++n;
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
	if (!player.Seated())
		return;
	if (m_bridge.UnseatRemotePed)
		m_bridge.UnseatRemotePed(player);
	player.seatedVehicleNetId = INVALID_NETID;
}

void Client::ClearRoster() {
	m_localPlayerId       = 0xFF;
	m_localVehicleNetId   = INVALID_NETID;
	m_vehicleClaimPending = false;
	// A new session hasn't been told anything about us yet.
	m_sentModelId         = 0xFFFF;
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
	for (RemoteVehicle &v : m_vehicles) {
		if (v.poolHandle >= 0 && m_bridge.DespawnRemoteVehicle)
			m_bridge.DespawnRemoteVehicle(v);
		v = RemoteVehicle{};
	}
}

void Client::PreFrame() {
	const bool connected = m_net.IsConnected();
	if (m_wasConnected && !connected) {
		// Everything in the roster now refers to a session that's gone.
		Log("client: connection lost, clearing %u remote player(s)", RemoteCount());
		ClearRoster();
	}
	m_wasConnected = connected;

	m_scratch.clear();
	m_net.DrainInbound(m_scratch);
	for (const Message &msg : m_scratch)
		HandleMessage(msg);

	UpdateRemotes();
	UpdateRemoteVehicles();
	// Runs after both spawn passes, so a ped and a car appearing on the same
	// frame get seated that same frame.
	UpdateRemoteSeats();
}

void Client::PostFrame() {
	if (!m_net.IsConnected())
		return;
	// Wall clock, not frames - the target install runs at 60 FPS and 60/25
	// isn't an integer (docs/compat.md §2.4). This runs every frame, not at
	// the send rate: it happens after CGame::Process and before the frame
	// draws, and its job is undoing the frame of local physics that just
	// happened to a car somebody else is driving. Has to run on every frame
	// physics ran, not just the frames we happen to be sending on.
	CorrectRemoteVehicles();

	// Above the rate limiter on purpose. A shot is an event, and the limiter
	// is there to thin out a *stream* - holding a muzzle flash back for up
	// to 40ms would let a second shot overtake it on a reliable-ordered
	// channel, and a burst would arrive as one clump.
	SendLocalCombat();

	if (!m_sendRate.Ready(WallClock::NowMs()))
		return;
	SendLocalModel();
	SendLocalState();
	SendLocalVehicle();
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
	} else if (const S_PlayerState *p = msg.as<S_PlayerState>()) {
		OnPlayerState(*p);
	} else if (const S_VehicleSpawn *p = msg.as<S_VehicleSpawn>()) {
		OnVehicleSpawn(*p);
	} else if (const S_VehicleDespawn *p = msg.as<S_VehicleDespawn>()) {
		OnVehicleDespawn(*p);
	} else if (const S_VehicleState *p = msg.as<S_VehicleState>()) {
		OnVehicleState(*p);
	} else if (const S_EnterVehicle *p = msg.as<S_EnterVehicle>()) {
		OnEnterVehicle(*p);
	} else if (const S_ExitVehicle *p = msg.as<S_ExitVehicle>()) {
		OnExitVehicle(*p);
	} else if (const S_Shot *p = msg.as<S_Shot>()) {
		OnShot(*p);
	} else if (const S_Explosion *p = msg.as<S_Explosion>()) {
		OnExplosion(*p);
	}
	// Anything else isn't handled yet, and stays silently ignored rather
	// than logged - the server already sends world state and chat, and a
	// per-packet log line at 25 Hz would drown out the one message that
	// actually matters.
}

void Client::OnWelcome(const S_Welcome &pkt) {
	if (pkt.reject != 0) {
		Log("client: server rejected the connection (reason %u)", pkt.reject);
		return;
	}
	Log("client: joined as player %u (netId %u), %u slots, %u Hz, %02u:%02u",
	    pkt.playerId, pkt.netId, pkt.maxPlayers, pkt.snapshotHz, pkt.hour, pkt.minute);

	// A welcome starts a session, so anything left from a previous one is stale.
	ClearRoster();
	m_localPlayerId = pkt.playerId;
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

	Log("client: %s joined as player %u", p.nick.c_str(), p.playerId);

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
	if (p.poolHandle >= 0) {
		UnseatPlayer(p);
		if (m_bridge.DespawnRemote)
			m_bridge.DespawnRemote(p);
	}
	p = RemotePlayer{};
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

	if (p.poolHandle >= 0) {
		UnseatPlayer(p);
		if (m_bridge.DespawnRemote)
			m_bridge.DespawnRemote(p);
		p.poolHandle = -1;
	}

	// A new ped starts with none of this applied, same as the first spawn.
	p.appliedAnimId  = ANIM_NONE;
	p.appliedAnimId2 = ANIM_NONE;
	p.appliedWeapon  = 0xFFFF;

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
	p.interp.Push(pkt.hdr.sendTimeMs, pkt.body.pos, pkt.body.heading, pkt.body.moveSpeed);
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
		Pose       pose;
		const bool havePose = p.interp.SampleDelayed(nowMs, pose);

		// Finish the two-phase spawn once the streamer has the model AND we
		// know where to put it.
		if (p.poolHandle < 0 && p.spawnPending && havePose && m_bridge.IsModelReady &&
		    m_bridge.SpawnRemote) {
			if (m_bridge.IsModelReady(p.modelId) && m_bridge.SpawnRemote(p)) {
				p.spawnPending = false;
				Log("client: spawned %s (handle %d)", p.nick.c_str(), p.poolHandle);
			}
		}

		if (p.poolHandle < 0 || !m_bridge.ApplyRemotePose)
			continue;

		if (havePose)
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

void Client::SendLocalState() {
	if (!m_bridge.SampleLocalPlayer)
		return;

	PlayerStateBody body{};
	if (!m_bridge.SampleLocalPlayer(body))
		return;   // no player right now - menus, loading, a cutscene

	C_PlayerState pkt;
	InitHeader(pkt, WallClock::NowMs());
	pkt.body = body;
	m_net.Send(pkt, CH_SNAPSHOT);
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

	for (uint8_t i = 0; i < n; ++i) {
		if (events[i].kind == CombatEvent::SHOT) {
			C_Shot out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].shot;
			m_net.Send(out, CH_EVENT);
		} else {
			C_Explosion out;
			InitHeader(out, WallClock::NowMs());
			out.body = events[i].explosion;
			m_net.Send(out, CH_EVENT);
		}
	}
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

void Client::OnVehicleSpawn(const S_VehicleSpawn &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/true);
	if (!v)
		return;

	v->modelId = pkt.modelId;
	v->colour1 = pkt.colour1;
	v->colour2 = pkt.colour2;

	// The spawn packet carries a position, so unlike a player join it's
	// enough on its own to place the vehicle. Seeding `last` from it lets
	// the spawn happen before the first state snapshot arrives, without
	// repeating the ped mistake of creating an entity at the origin.
	v->last.netId = pkt.netId;
	v->last.pos   = pkt.pos;
	v->last.rot   = pkt.rot;
	v->last.health = 1000.0f;
	v->haveState   = true;
	v->spawnPending = true;

	if (m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.modelId);

	Log("client: vehicle %u joined (model %u)", pkt.netId, pkt.modelId);
}

void Client::OnVehicleDespawn(const S_VehicleDespawn &pkt) {
	RemoteVehicle *v = VehicleSlot(pkt.netId, /*createIfMissing=*/false);
	if (!v)
		return;

	// Everyone gets out before the car goes. See UnseatPlayer.
	for (RemotePlayer &p : m_players)
		if (p.active && p.seatedVehicleNetId == pkt.netId)
			UnseatPlayer(p);

	if (v->poolHandle >= 0 && m_bridge.DespawnRemoteVehicle)
		m_bridge.DespawnRemoteVehicle(*v);
	*v = RemoteVehicle{};
	Log("client: vehicle %u left", pkt.netId);
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
	v->last      = pkt.body;
	v->haveState = true;
	v->interp.Push(pkt.hdr.sendTimeMs, pkt.body.pos, pkt.body.rot, pkt.body.moveSpeed);
}

void Client::UpdateRemoteVehicles() {
	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active)
			continue;

		if (v.poolHandle < 0 && v.spawnPending && v.haveState && m_bridge.IsModelReady &&
		    m_bridge.SpawnRemoteVehicle) {
			if (m_bridge.IsModelReady(v.modelId) && m_bridge.SpawnRemoteVehicle(v)) {
				v.spawnPending = false;
				Log("client: spawned vehicle %u (handle %d)", v.netId, v.poolHandle);
			}
		}

		if (v.poolHandle < 0 || !v.haveState || !m_bridge.ApplyRemoteVehicle)
			continue;

		// Controls, health and flags only. Transform isn't written here -
		// anything written before CGame::Process is just what local physics
		// starts from, not what actually gets drawn. See CorrectRemoteVehicles.
		m_bridge.ApplyRemoteVehicle(v, v.last);
	}
}

void Client::CorrectRemoteVehicles() {
	if (!m_bridge.CorrectRemoteVehicle)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteVehicle &v : m_vehicles) {
		if (!v.active || v.poolHandle < 0)
			continue;

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
		// The reply to our own claim - the only thing that tells us what
		// netId the session gave to the car we're sitting in.
		m_localVehicleNetId   = pkt.body.netId;
		m_vehicleClaimPending = false;
		Log("client: our vehicle is net %u", pkt.body.netId);
		return;
	}

	if (pkt.playerId >= MAX_PLAYERS)
		return;

	RemotePlayer &p = m_players[pkt.playerId];
	if (!p.active)
		return;

	if (pkt.body.seat == 0)
		if (RemoteVehicle *v = VehicleSlot(pkt.body.netId, false))
			v->driverPlayerId = pkt.playerId;

	// Recorded, not acted on. Either end of the pair might still be
	// streaming in - this event is reliable and arrives all at once, while
	// the ped and the car each take as long as their model does to load.
	// UpdateRemoteSeats carries it out on the first frame both exist.
	p.seatVehicleNetId = pkt.body.netId;
	p.seatIndex        = pkt.body.seat;
}

void Client::OnExitVehicle(const S_ExitVehicle &pkt) {
	if (pkt.playerId == m_localPlayerId) {
		m_localVehicleNetId   = INVALID_NETID;
		m_vehicleClaimPending = false;
		return;
	}
	if (RemoteVehicle *v = VehicleSlot(pkt.netId, false))
		if (v->driverPlayerId == pkt.playerId)
			v->driverPlayerId = 0xFF;

	if (pkt.playerId < MAX_PLAYERS)
		m_players[pkt.playerId].seatVehicleNetId = INVALID_NETID;
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
void Client::UpdateRemoteSeats() {
	for (RemotePlayer &p : m_players) {
		if (!p.active)
			continue;

		// No ped means nothing is seated, whatever we thought before. This
		// also covers recovery from a ped the engine took away underneath
		// us - ResolveRemote clears the handle and re-arms the spawn, and
		// the new ped gets seated again on the next pass through here.
		if (p.poolHandle < 0) {
			p.seatedVehicleNetId = INVALID_NETID;
			continue;
		}

		// What the session asks for, reduced to what's actually possible. A
		// car that hasn't spawned yet isn't a seat, so the request just
		// stands and we try again next frame.
		uint16_t       want = p.seatVehicleNetId;
		RemoteVehicle *v =
		    want != INVALID_NETID ? VehicleSlot(want, /*createIfMissing=*/false)
		                          : nullptr;
		if (!v || v->poolHandle < 0)
			want = INVALID_NETID;

		if (p.seatedVehicleNetId == want)
			continue;

		// Always out of the old seat first, even when the move goes straight
		// from one car into another.
		UnseatPlayer(p);

		if (want == INVALID_NETID || !m_bridge.SeatRemotePed)
			continue;

		if (m_bridge.SeatRemotePed(p, *v, p.seatIndex)) {
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
		if (p.poolHandle >= 0 && v->poolHandle >= 0)
			p.seatVehicleNetId = INVALID_NETID;
	}
}

void Client::SendLocalVehicle() {
	// Not in a car - if we just were, tell the session we got out. One
	// packet on the transition, not a stream of "still not driving".
	if (!m_bridge.SampleLocalVehicle || !m_bridge.SampleLocalVehicleIdentity)
		return;

	uint16_t model = 0;
	uint8_t  c1 = 0, c2 = 0;
	Vec3     pos{};
	Quat     rot{0.0f, 0.0f, 0.0f, 1.0f};
	const bool driving = m_bridge.SampleLocalVehicleIdentity(model, c1, c2, pos, rot);

	if (!driving) {
		if (m_localVehicleNetId != INVALID_NETID) {
			C_ExitVehicle out;
			InitHeader(out, WallClock::NowMs());
			out.netId = m_localVehicleNetId;
			m_net.Send(out, CH_EVENT);
			m_localVehicleNetId = INVALID_NETID;
		}
		m_vehicleClaimPending = false;
		return;
	}

	// Driving something the session hasn't named yet. Claim it once and
	// wait - the claim is reliable so it'll get there, and resending it
	// every frame would just register the same car over and over.
	if (m_localVehicleNetId == INVALID_NETID) {
		if (m_vehicleClaimPending)
			return;
		C_EnterVehicle out;
		InitHeader(out, WallClock::NowMs());
		out.body.netId   = INVALID_NETID;
		out.body.seat    = 0;
		out.body.jack    = 0;
		out.body.modelId = model;
		out.body.colour1 = c1;
		out.body.colour2 = c2;
		out.body.pad     = 0;
		out.body.pos     = pos;
		out.body.rot     = rot;
		m_net.Send(out, CH_EVENT);
		m_vehicleClaimPending = true;
		Log("client: claiming the car we just got into (model %u)", model);
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
}

} // namespace coopiii
