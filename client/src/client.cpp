#include "client.h"

#include "game/wanted.h"
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
	// An entry that is still in flight has to come off first, and it has to
	// come off through the engine: the door it claimed is held in the car's
	// m_nGettingInFlags until QuitEnteringCar gives it back, and a door
	// nobody gives back is a door nobody can ever use again. This is why
	// every caller can go on calling UnseatPlayer and nothing else.
	if (player.Entering()) {
		if (m_bridge.AbandonSeatRemotePed)
			m_bridge.AbandonSeatRemotePed(player);
		player.enteringVehicleNetId = INVALID_NETID;
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
	// Nobody's clock is ours to follow once the session is gone. The sky
	// stays pinned where the last host left it, deliberately: unpinning it
	// here would change the weather on a player who has just been
	// disconnected and is about to reconnect to the same session.
	m_hostPlayerId        = INVALID_PLAYER;
	m_localVehicleNetId   = INVALID_NETID;
	m_vehicleClaimPending = false;
	m_claimRetryAtMs      = 0;
	// A new session hasn't been told anything about us yet.
	m_sentModelId         = 0xFFFF;
	// Nor has it been told we died. A death announced to the old session is
	// not a death this one knows about, and leaving the flag set would mean
	// the first respawn after a reconnect gets announced on its own.
	m_deathAnnounced      = false;
	m_lastAttackerNetId   = INVALID_NETID;
	m_lastAttackerMs      = 0;
	// The wanted bookkeeping is about a session, so it goes with one, and the
	// rule goes back to the default - a client with no session must not go on
	// clamping its own player to zero because the server it has just lost had
	// the wanted level switched off.
	m_wantedRule          = WANTED_RULE_PERPLAYER;
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
	for (RemoteVehicle &v : m_vehicles) {
		// Not the car we claimed. That one is this engine's own traffic car,
		// very possibly with the local player sitting in it; the session
		// ending is not a reason to run a destructor over it. Same rule the
		// pedestrian half already keeps for the peds this machine hosts.
		if (v.poolHandle >= 0 && !v.ours && m_bridge.DespawnRemoteVehicle)
			m_bridge.DespawnRemoteVehicle(v);
		v = RemoteVehicle{};
	}
	// And every replica of somebody else's pedestrian, for the same reason.
	// The peds this machine *hosts* are untouched: those are the engine's own
	// pedestrians and they carry on being pedestrians after the session ends.
	ClearAmbientPeds();
	ClearAmbientCars();
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
		ClearRoster();
	}
	m_wasConnected = connected;

	m_scratch.clear();
	m_net.DrainInbound(m_scratch);
	for (const Message &msg : m_scratch)
		HandleMessage(msg);

	UpdateRemotes();
	UpdateRemoteVehicles();
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
	if (!m_net.IsConnected())
		return;
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

	// Above the rate limiter on purpose. A shot is an event, and the limiter
	// is there to thin out a *stream* - holding a muzzle flash back for up
	// to 40ms would let a second shot overtake it on a reliable-ordered
	// channel, and a burst would arrive as one clump.
	SendLocalCombat();

	// Above the limiter too, and for the same reason: a pedestrian born and
	// reaped between two 25 Hz ticks would otherwise be announced after it had
	// already stopped existing. These are reliable events, not a stream.
	SendLocalAmbientPeds();
	SendLocalAmbientCars();

	// And the unowned cars our own engine just destroyed. Above the limiter
	// with the rest of the reliable events, and NOT beside
	// SendLocalVehicleBlasts below it: that one is held back until the send
	// tick because the server checks it against who it thinks is driving, and
	// this one has nothing to be checked against.
	SendUnownedBlasts();

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

	// Also above the limiter, with a 1 Hz one of its own inside it. A game
	// minute is a real second, so anything faster is repetition.
	SendLocalWorld();

	if (!m_sendRate.Ready(WallClock::NowMs()))
		return;
	SendLocalModel();
	SendLocalState();
	// After the snapshot, not before. The snapshot carries the held weapon's
	// count, and sending the stored slots first would have a receiver apply
	// a stale hand before the current one arrived.
	SendLocalAmmo();
	// Blasts before the exit, and the order is load-bearing. Both are
	// reliable and ordered, and the server only takes a blast from the player
	// it believes is driving that car - so an exit that overtook it would
	// have the server throw the blast away as coming from nobody.
	SendLocalVehicleBlasts();
	SendLocalVehicle();
	// After SendLocalVehicle, because that is what claims a car and fills in
	// m_localVehicleNetId - a dent reported before the session has a name for
	// the car has nothing to name it with.
	SendLocalVehicleDamage();
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
	} else if (const S_UnownedBlowUp *p = msg.as<S_UnownedBlowUp>()) {
		OnUnownedBlowUp(*p);
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
	} else if (const S_PedSpawn *p = msg.as<S_PedSpawn>()) {
		OnPedSpawn(*p);
	} else if (const S_PedDespawn *p = msg.as<S_PedDespawn>()) {
		OnPedDespawn(*p);
	} else if (const S_PedBodyPart *p = msg.as<S_PedBodyPart>()) {
		OnPedBodyPart(*p);
	} else if (const S_PedDeath *p = msg.as<S_PedDeath>()) {
		OnPedDeath(*p);
	} else if (const S_CarSpawn *p = msg.as<S_CarSpawn>()) {
		OnCarSpawn(*p);
	} else if (const S_CarDespawn *p = msg.as<S_CarDespawn>()) {
		OnCarDespawn(*p);
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
	m_localPlayerId = pkt.playerId;
	m_localNetId    = pkt.netId;
	// After ClearRoster, not before. It puts the wanted rule back to the
	// default, which is what a client with no session wants and would quietly
	// undo the line above if this were assigned first - the whole feature
	// then runs as per-player however the server is configured, which is a
	// bug that shows up only in a session somebody deliberately set to
	// `shared` or `off`.
	m_wantedRule    = rule;

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
	// An animated entry already in flight, driven before anything else: until
	// it resolves the player is neither on foot nor riding, and the checks
	// below would read them as on foot and start a second one.
	if (m_pendingSeatNetId != INVALID_NETID && m_bridge.PollLocalSeatEntry) {
		const int32_t walking = m_bridge.PollLocalSeatEntry();
		if (walking == SEAT_LOCAL_WALKING)
			return;

		const uint16_t netId = m_pendingSeatNetId;
		m_pendingSeatNetId   = INVALID_NETID;
		if (walking >= 0)
			AnnounceLocalSeat(netId, static_cast<uint8_t>(walking));
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

	const int32_t seat = m_bridge.SeatLocalPlayerIn(best->poolHandle);
	if (seat == SEAT_LOCAL_WALKING) {
		// The engine is walking us to the door. The session is told nothing
		// yet: announcing the seat now would sit a passenger down on every
		// other screen while the door here is still closed, which is the
		// teleport this whole change exists to remove.
		m_pendingSeatNetId = best->netId;
		Log("client: walking to vehicle %u to ride in it, %.1f m away when we "
		    "asked",
		    best->netId, bestDist);
		return;
	}
	if (seat < 0)
		return;   // game/seat.cpp said why

	AnnounceLocalSeat(best->netId, static_cast<uint8_t>(seat));
}

// One place that tells the session we are sitting in somebody else's car, so
// the warp fallback and the animated entry cannot drift apart in what they
// send or when they send it.
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

	return false;
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
		if (p.active && p.InvolvedWith(pkt.netId))
			UnseatPlayer(p);

	// Not one of our own. The session dropping a car it can no longer track
	// is not the same statement as "this CVehicle should be destroyed", and
	// for a car our engine made it is not ours to make. See RemoteVehicle::ours.
	if (v->poolHandle >= 0 && !v->ours && m_bridge.DespawnRemoteVehicle)
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

		if (v.poolHandle < 0 || !v.haveState)
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
		if (engineSaysOurs && !DrivenLocally(v)) {
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
		if (DrivenLocally(v) || engineSaysOurs)
			continue;

		// Controls, health and flags only. Transform isn't written here -
		// anything written before CGame::Process is just what local physics
		// starts from, not what actually gets drawn. See CorrectRemoteVehicles.
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
		if (v.driverPlayerId == 0xFF && m_bridge.RestRemoteVehicle)
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
		if (DrivenLocally(v) ||
		    (m_bridge.LocalDrivesVehicle && m_bridge.LocalDrivesVehicle(v)))
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
		if (RemoteVehicle *v = VehicleSlot(pkt.body.netId, false))
			v->driverPlayerId = pkt.playerId;

	// Recorded, not acted on. Either end of the pair might still be
	// streaming in - this event is reliable and arrives all at once, while
	// the ped and the car each take as long as their model does to load.
	// UpdateRemoteSeats carries it out on the first frame both exist.
	p.seatVehicleNetId = pkt.body.netId;
	p.seatIndex        = pkt.body.seat;
	// A fresh statement about where this player is sitting earns one fresh
	// attempt at the door-opening version of getting there. Without this
	// reset a single timed-out entry would spend the player's remaining
	// session warping, and with the reset anywhere else an entry that cannot
	// finish is retried forever.
	p.seatAnimSpent    = false;

	// The jack byte is read and deliberately not acted on. Getting into a
	// car somebody is already in is a different animation in the engine -
	// CPed::SetCarJack, which addresses.h records - and CoopIII does not
	// play it, for two reasons that are both about this being an observer.
	//
	// The first is mechanical, and narrower than it was first written down
	// as. SetCarJack returns without doing anything when the car's
	// VehicleCreatedBy is MISSION_VEHICLE, which is what every car CoopIII
	// creates is - but that bail sits *after* the CPed::IsPlayer call at
	// 0x004E0310 and the `jne` at 0x004E0319 jumps over it, so it applies to
	// a remote ped and not to the local player pressing F. Since the ped that
	// would play this animation is always a CCivilianPed, the animation is
	// indeed unavailable on exactly the cars a session has. addresses.h has
	// the disassembly and the correction.
	//
	// The second matters more. SetCarJack's animation chain ends by dragging
	// the ped in the seat out of it, which would be this machine deciding
	// that some *other* player left a car. That is the host-authoritative
	// rule backwards. What the seat actually needs is handled without the
	// flag: UpdateRemoteSeats takes whoever is in the seat out of it before
	// putting the named player in, because the session has already said who
	// is sitting there and two peds cannot both be the driver. The loser's
	// own machine sends their exit a moment later and the two agree.
	//
	// Nothing sets the byte on the way out either (SendLocalVehicle writes a
	// zero into all three sites), so this is documenting a field that is
	// currently always false rather than ignoring live information.
	(void)pkt.body.jack;
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
		Log("client: we got out of vehicle %u", m_localVehicleNetId);
		m_localVehicleNetId   = INVALID_NETID;
		m_vehicleClaimPending = false;
		m_claimRetryAtMs      = 0;
		return;
	}
	if (RemoteVehicle *v = VehicleSlot(pkt.netId, false))
		if (v->driverPlayerId == pkt.playerId)
			v->driverPlayerId = 0xFF;

	if (pkt.playerId < MAX_PLAYERS) {
		m_players[pkt.playerId].seatVehicleNetId = INVALID_NETID;
		Log("client: %s got out of vehicle %u",
		    m_players[pkt.playerId].nick.c_str(), pkt.netId);
	}
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
			const uint8_t progress =
			    (p.enteringVehicleNetId == want && m_bridge.PollSeatRemotePed)
			        ? m_bridge.PollSeatRemotePed(p, *v, p.seatIndex)
			        : SEAT_LOST;   // the session changed its mind, or no bridge

			if (progress == SEAT_DONE) {
				p.seatedVehicleNetId   = want;
				p.enteringVehicleNetId = INVALID_NETID;
				Log("client: %s opened the door of vehicle %u and got in "
				    "(seat %u)",
				    p.nick.c_str(), want, p.seatIndex);
				continue;
			}
			if (progress == SEAT_RUNNING && now < p.enterDeadlineMs)
				continue;   // still walking to the handle

			// Refused, interrupted, or out of time. Take it off the ped -
			// which is what gives the door back to the car - and fall
			// through to the seating below, in this same pass. Nothing here
			// retries: `seatAnimSpent` is already true, so what follows is
			// the warp.
			if (m_bridge.AbandonSeatRemotePed)
				m_bridge.AbandonSeatRemotePed(p);
			p.enteringVehicleNetId = INVALID_NETID;
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
		if (!p.seatAnimSpent && m_bridge.BeginSeatRemotePed) {
			p.seatAnimSpent = true;
			if (m_bridge.BeginSeatRemotePed(p, *v, p.seatIndex)) {
				p.enteringVehicleNetId = want;
				p.enterDeadlineMs      = now + m_seatAnimTimeoutMs;
				continue;
			}
		}

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
	const bool      driving = m_bridge.SampleLocalVehicleIdentity(id);

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
	// Not ours to show if we never built it: a replica still waiting on its
	// model, or a ped this machine hosts itself, whose limb our own engine
	// took off already. The server does not send that one back, and this is
	// the guard for when it does.
	if (!ped || ped->poolHandle < 0 || !m_bridge.RemoveAmbientBodyPart)
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

		// The standing instruction, not the act. UpdateAmbientPedSeats is
		// what carries it out, on whatever frame both halves exist.
		ped->seatVehicleNetId = in.vehicleNetId;
		ped->seatIndex        = in.seat;
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

		// A seated ped is positioned by CWorld::Process from its car's
		// matrix, every frame (docs/protocol.md §1.13.2). Writing a position
		// over the top of that is what would drag him half out of the seat
		// for exactly the part of the frame physics and collision look at -
		// the same rule, and the same reason, as ApplyRemotePose's early
		// return for a seated player.
		if (ped.Seated())
			continue;

		Pose at;
		if (!ped.interp.SampleDelayed(nowMs, at)) {
			// Nothing in the buffer: either the ped has never been streamed,
			// or it is not one of the twelve its owner is streaming right now
			// (protocol.h, MAX_PED_STATES). Both mean hold it where the
			// session last put it, which for a ped is a person standing
			// still - the step 2 behaviour, now the far band of §2.1's
			// rate-by-distance rather than the whole feature.
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

void Client::SendHostedPedStates() {
	if (!m_bridge.SampleHostedPeds)
		return;

	// 10 Hz, on a limiter of its own. The same argument as the traffic
	// stream's and the same number, arrived at differently: a car is fast
	// enough that 10 Hz is already coarse, and a pedestrian is slow enough
	// that 10 Hz is generous. What buys the ped stream its place in the
	// budget is the row being 24 bytes rather than 44, not the clock.
	constexpr uint32_t PED_STATE_INTERVAL_MS = 100;
	const uint32_t nowMs = WallClock::NowMs();
	if (nowMs - m_lastPedStateMs < PED_STATE_INTERVAL_MS)
		return;
	m_lastPedStateMs = nowMs;

	C_PedStates out;
	InitHeader(out, nowMs);
	out.count = static_cast<uint8_t>(
	    m_bridge.SampleHostedPeds(out.peds, MAX_PED_STATES));
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

		car->interp.Push(pkt.hdr.sendTimeMs, in.pos, in.rot, in.velocity);
		car->last.pos = in.pos;
		car->last.rot = in.rot;
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
		if (m_bridge.SpawnAmbientCarReplica(car))
			car.spawnPending = false;
	}
}

void Client::CorrectAmbientCars() {
	if (!m_bridge.CorrectAmbientCarReplica)
		return;

	const uint32_t nowMs = WallClock::NowMs();

	for (RemoteAmbientCar &car : m_cars) {
		if (!car.active || car.poolHandle < 0)
			continue;

		VehicleTransform at;
		if (car.interp.SampleDelayed(nowMs, at)) {
			m_bridge.CorrectAmbientCarReplica(car, at);
			continue;
		}

		// Nothing in the buffer: either the car has never been streamed, or
		// it is not one of the eight its owner is streaming right now
		// (protocol.h, MAX_CAR_STATES). Both mean the same thing here - hold
		// it at the last transform the session gave us rather than hand it to
		// the local suspension, which walks a standing car down a hill over a
		// couple of minutes.
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
	// the rate has to fall off with distance rather than be flat. This is the
	// crude version of that - a slower clock for everything ambient, and only
	// the eight nearest cars inside each tick. Measuring it properly against
	// a real session is step 5.
	constexpr uint32_t CAR_STATE_INTERVAL_MS = 100;
	const uint32_t nowMs = WallClock::NowMs();
	if (nowMs - m_lastCarStateMs < CAR_STATE_INTERVAL_MS)
		return;
	m_lastCarStateMs = nowMs;

	C_CarStates out;
	InitHeader(out, nowMs);
	out.count = static_cast<uint8_t>(
	    m_bridge.SampleHostedCars(out.cars, MAX_CAR_STATES));
	if (out.count == 0)
		return;
	m_net.Send(out, CH_SNAPSHOT);
}

} // namespace coopiii
