#include "client.h"

#include "log.h"

#include <cmath>
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
	m_localNetId          = INVALID_NETID;
	// Nobody's clock is ours to follow once the session is gone. The sky
	// stays pinned where the last host left it, deliberately: unpinning it
	// here would change the weather on a player who has just been
	// disconnected and is about to reconnect to the same session.
	m_hostPlayerId        = INVALID_PLAYER;
	m_localVehicleNetId   = INVALID_NETID;
	m_vehicleClaimPending = false;
	// A new session hasn't been told anything about us yet.
	m_sentModelId         = 0xFFFF;
	// Nor has it been told we died. A death announced to the old session is
	// not a death this one knows about, and leaving the flag set would mean
	// the first respawn after a reconnect gets announced on its own.
	m_deathAnnounced      = false;
	m_lastAttackerNetId   = INVALID_NETID;
	m_lastAttackerMs      = 0;
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

	// Also above the limiter, with a 1 Hz one of its own inside it. A game
	// minute is a real second, so anything faster is repetition.
	SendLocalWorld();

	if (!m_sendRate.Ready(WallClock::NowMs()))
		return;
	SendLocalModel();
	SendLocalState();
	// Blasts before the exit, and the order is load-bearing. Both are
	// reliable and ordered, and the server only takes a blast from the player
	// it believes is driving that car - so an exit that overtook it would
	// have the server throw the blast away as coming from nobody.
	SendLocalVehicleBlasts();
	SendLocalVehicle();
	TickPassengerSeat();
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
	} else if (const S_VehicleBlowUp *p = msg.as<S_VehicleBlowUp>()) {
		OnVehicleBlowUp(*p);
	} else if (const S_EnterVehicle *p = msg.as<S_EnterVehicle>()) {
		OnEnterVehicle(*p);
	} else if (const S_ExitVehicle *p = msg.as<S_ExitVehicle>()) {
		OnExitVehicle(*p);
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
	}
	// Anything else isn't handled yet, and stays silently ignored rather
	// than logged - the server already sends chat, and a per-packet log line
	// at 25 Hz would drown out the one message that actually matters.
}

void Client::OnWelcome(const S_Welcome &pkt) {
	if (pkt.reject != 0) {
		Log("client: server rejected the connection (reason %u)", pkt.reject);
		return;
	}
	m_friendlyFire = (pkt.flags & SESSION_FRIENDLY_FIRE) != 0;
	Log("client: joined as player %u (netId %u), %u slots, %u Hz, %02u:%02u, "
	    "friendly fire %s",
	    pkt.playerId, pkt.netId, pkt.maxPlayers, pkt.snapshotHz, pkt.hour, pkt.minute,
	    m_friendlyFire ? "on" : "off");

	// A welcome starts a session, so anything left from a previous one is stale.
	ClearRoster();
	m_localPlayerId = pkt.playerId;
	m_localNetId    = pkt.netId;

	// The one part of friendly fire the server can't enforce by refusing to
	// relay: an explosion is replayed locally at a position its owner chose,
	// so this machine is the one that has to decline the blast. See
	// SessionFlags in protocol.h.
	if (m_bridge.SetFriendlyFire)
		m_bridge.SetFriendlyFire(m_friendlyFire);
	SetHost(pkt.hostPlayerId);

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
		p.seedPose.pos     = pkt.pos;
		p.seedPose.heading = pkt.heading;
		p.haveSeedPose     = true;
	}

	Log("client: %s joined as player %u (%.0f hp, weapon %u%s%s)", p.nick.c_str(),
	    p.playerId, pkt.health, pkt.weapon, p.dead ? ", dead" : "",
	    (pkt.flags & PJF_POS_VALID) ? "" : ", position unknown");

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
	// Their own timeline has started, so the session's starting guess is done
	// with. Dropped here rather than left as a fallback on purpose: falling
	// back to it during a later stall would yank the ped back to wherever
	// they were standing when we joined, which is a teleport and not a
	// recovery.
	p.haveSeedPose = false;
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
			if (m_bridge.IsModelReady(p.modelId) && m_bridge.SpawnRemote(p)) {
				p.spawnPending = false;
				Log("client: spawned %s (handle %d)", p.nick.c_str(), p.poolHandle);
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

	// Recorded, not carried out. UpdateRemotes does the killing, on the first
	// frame there is a ped to kill - which is this one when their ped is
	// already up, and a later one when the death caught them mid-stream. The
	// old "if there is a ped, kill it" simply lost the death in that second
	// case and left a player walking around on zero health.
	p.dead         = true;
	p.deathAnimId  = pkt.animId;
	p.deathApplied = false;
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
	const LifeEvent event = LifeEventFor(body.health, m_deathAnnounced);
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

	// Alive again. GTA III resurrects the same ped rather than making a new
	// one, so from here a respawn is just health coming back - the position
	// that goes with it is wherever the hospital put us.
	m_deathAnnounced    = false;
	m_lastAttackerNetId = INVALID_NETID;
	m_lastAttackerMs    = 0;

	C_Respawn out;
	InitHeader(out, WallClock::NowMs());
	out.body.pos     = body.pos;
	out.body.heading = body.heading;
	m_net.Send(out, CH_EVENT);
	Log("client: we respawned at %.1f %.1f %.1f", body.pos.x, body.pos.y, body.pos.z);
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

	const int32_t seat = m_bridge.SeatLocalPlayerIn(best->poolHandle);
	if (seat < 0)
		return;   // game/seat.cpp said why

	C_EnterVehicle out;
	InitHeader(out, WallClock::NowMs());
	out.body       = EnterVehicleBody{};
	out.body.netId = best->netId;
	out.body.seat  = static_cast<uint8_t>(seat);
	out.body.jack  = 0;
	m_net.Send(out, CH_EVENT);

	m_localSeatNetId = best->netId;
	Log("client: riding in vehicle %u, seat %d, %.1f m away when we asked",
	    best->netId, seat, bestDist);
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
	if (!IsHost() || !m_bridge.SampleWorld)
		return;
	if (!m_worldRate.Ready(WallClock::NowMs()))
		return;

	WorldState mine;
	if (!m_bridge.SampleWorld(mine))
		return;   // no world to report - a loading screen, the menu

	C_WorldState out;
	InitHeader(out, WallClock::NowMs());
	out.body.hour       = mine.hour;
	out.body.minute     = mine.minute;
	out.body.weather    = mine.weather;
	out.body.weatherOld = mine.weatherOld;
	m_net.Send(out, CH_EVENT);
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

RemoteVehicle *Client::ObservedVehicleWeAreDriving() {
	if (!m_bridge.SampleLocalVehicleHandle)
		return nullptr;

	const int32_t handle = m_bridge.SampleLocalVehicleHandle();
	if (handle < 0)
		return nullptr;

	// Matched on the engine's own reference, not on model and position.
	// Identity has to be exact here: two identical parked cars side by side
	// are an ordinary sight in Liberty City, and picking the wrong one
	// would register the session's car under a second netId and leave the
	// real one pinned - the same bug, arrived at more cleverly.
	for (RemoteVehicle &v : m_vehicles)
		if (v.active && v.poolHandle >= 0 && v.poolHandle == handle)
			return &v;
	return nullptr;
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

	if (m_bridge.RequestModel)
		m_bridge.RequestModel(pkt.modelId);

	Log("client: vehicle %u joined (model %u, %.0f hp%s, extras %d/%d)", pkt.netId,
	    pkt.modelId, pkt.health, (pkt.flags & VEH_WRECKED) ? ", wrecked" : "",
	    static_cast<int>(pkt.extra1), static_cast<int>(pkt.extra2));
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
	// A wreck takes no more orders. Its owner is dead or out of it, so
	// anything still arriving on this channel was sampled before the blast,
	// and applying it would relight a burnt-out car.
	if (v->destroyed)
		return;

	v->last      = pkt.body;
	v->haveState = true;
	v->interp.Push(pkt.hdr.sendTimeMs, pkt.body.pos, pkt.body.rot, pkt.body.moveSpeed);
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
		if (p.active && p.seatedVehicleNetId == pkt.body.netId)
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
				Log("client: spawned vehicle %u (handle %d)", v.netId, v.poolHandle);
			}
		}

		if (v.poolHandle < 0 || !v.haveState || !m_bridge.ApplyRemoteVehicle)
			continue;

		// Not onto a car we are driving ourselves. The row is still here
		// because the car is still in the session, but its condition is ours
		// to report now, and writing the last driver's health, gear and
		// engine flag back onto it every snapshot would be this machine
		// arguing with itself.
		if (DrivenLocally(v))
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
		if (DrivenLocally(v))
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
		// And a corpse sits in nothing. OnDeath already drops the standing
		// instruction, so this only catches a seating the session hands us
		// for somebody it also says is dead - which the server no longer
		// does, and which would be a warp into a car everybody else watched
		// them die outside of.
		if (p.dead)
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

	VehicleIdentity id{};
	const bool      driving = m_bridge.SampleLocalVehicleIdentity(id);

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
	if (RemoteVehicle *ours = VehicleSlot(m_localVehicleNetId, /*createIfMissing=*/false))
		ours->interp.Push(pkt.hdr.sendTimeMs, body.pos, body.rot, body.moveSpeed);
}

} // namespace coopiii
