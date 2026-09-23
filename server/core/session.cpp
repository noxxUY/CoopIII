#include "session.h"

#include "coopiii/net.h"   // InitHeader

#include <algorithm>
#include <cstring>

namespace coopiii {

void GameClock::Advance(uint32_t elapsedMs) {
	m_accumMs += elapsedMs;
	while (m_accumMs >= MS_PER_GAME_MINUTE) {
		m_accumMs -= MS_PER_GAME_MINUTE;
		if (++m_minute >= 60) {
			m_minute = 0;
			if (++m_hour >= 24)
				m_hour = 0;
		}
	}
}

bool GameClock::Set(uint8_t hour, uint8_t minute) {
	if (hour > 23 || minute > 59)
		return false;
	m_hour    = hour;
	m_minute  = minute;
	// Start the next minute from scratch. Keeping the leftover would let a
	// report land a fraction of a second before a rollover and tick the
	// minute straight back off again.
	m_accumMs = 0;
	return true;
}

Session::Session(uint8_t maxPlayers) : m_players(maxPlayers) {
	for (uint8_t i = 0; i < maxPlayers; ++i)
		m_players[i].id = i;
}

Player *Session::AddPlayer(uint32_t peer, const char *nick, uint16_t modelId,
                           uint16_t protocolVersion, RejectReason &reject) {
	if (protocolVersion != PROTOCOL_VERSION) {
		reject = REJECT_BAD_VERSION;
		return nullptr;
	}

	auto slot = std::find_if(m_players.begin(), m_players.end(),
	                         [](const Player &p) { return !p.active; });
	if (slot == m_players.end()) {
		reject = REJECT_FULL;
		return nullptr;
	}

	const uint8_t id = slot->id;
	*slot            = Player{};
	slot->active     = true;
	slot->peer       = peer;
	slot->id         = id;
	slot->netId      = AllocNetId();
	slot->nick       = SanitizeText(nick, NICK_LEN);
	slot->modelId    = modelId;

	reject = REJECT_NONE;
	PickHost();
	return &*slot;
}

uint8_t Session::RemovePeer(uint32_t peer) {
	Player *p = FindByPeer(peer);
	if (!p)
		return INVALID_PLAYER;

	const uint8_t id = p->id;

	// Whatever they were driving is now a parked car, not a car belonging to
	// a slot number that is about to be handed to somebody else. Slots are
	// reused, and a stale driverPlayerId would make the next player to fill
	// this one the owner of a car they have never seen - the same class of
	// mistake PickHost's "still here?" test exists to avoid.
	for (Vehicle &v : m_vehicles)
		if (v.active && v.driverPlayerId == id)
			v.driverPlayerId = INVALID_PLAYER;

	// And any car they were settling for the session (protocol.h,
	// S_VehicleCustody). Cleared rather than handed on, and that is a
	// decision: the only machine worth giving a driverless car to is one that
	// has it streamed in with the collision loaded around it, which is why
	// custody goes to the player who was just driving it in the first place.
	// Nobody else is known to be anywhere near it, and a custodian that
	// cannot see the car simulates it falling through an unloaded world and
	// reports the fall.
	//
	// So it goes back to the state this whole feature is the exception to:
	// nobody simulates it and every machine holds it where it stands. If it
	// was mid-fall when its custodian's socket closed it stays mid-fall,
	// which is exactly what happened before any of this existed and is no
	// worse for having been briefly better.
	for (Vehicle &v : m_vehicles)
		if (v.active && v.custodianPlayerId == id)
			v.custodianPlayerId = INVALID_PLAYER;

	// Their helicopters went with their engine. The observers find out from
	// the S_PlayerLeave the server sends anyway, and the slot is about to be
	// handed to somebody whose serials start again from 1.
	if (id < MAX_PLAYERS)
		m_helis[id] = OwnerHelis{};

	*p    = Player{};
	p->id = id;
	PickHost();
	return id;
}

// ---- keeping the session's copy current ------------------------------------

void Session::NotePlayerState(Player &p, const PlayerStateBody &body) {
	p.pos     = body.pos;
	p.heading = body.heading;
	p.havePos = true;

	// Condition, for the backfill and for nothing else. Health is recorded as
	// reported rather than being second-guessed against `alive`: the two come
	// from different packets and the machine that owns the player is the
	// authority on both.
	p.health = body.health;
	p.armour = body.armour;
	p.weapon = body.weapon;
}

void Session::NotePlayerDied(Player &p, uint16_t deathAnimId) {
	p.alive       = false;
	p.deathAnimId = deathAnimId;

	// Out of the car. Every client that was connected already did this when
	// the death arrived (client.cpp, OnDeath), so leaving the seat in the
	// session would mean the next joiner is the only machine in the session
	// that puts a corpse behind the wheel.
	NoteExitVehicle(p, p.vehicleNetId);
}

void Session::NotePlayerRespawned(Player &p, const Vec3 &pos, float heading) {
	p.alive       = true;
	p.deathAnimId = ANIM_NONE;
	p.pos         = pos;
	p.heading     = heading;
	p.havePos     = true;
	// The hospital hands back a full bar and takes the armour. Recorded so a
	// joiner in the gap before their next snapshot doesn't get told they are
	// still on the health they died with.
	p.health      = 100.0f;
	p.armour      = 0.0f;

	NoteExitVehicle(p, p.vehicleNetId);
}

// ---- who is sitting where --------------------------------------------------

uint8_t Session::NoteEnterVehicle(Player &p, Vehicle &v, uint8_t seat) {
	// Out of whatever they were in first, so nobody is ever recorded in two
	// cars at once. Stepping straight from one car into another is a single
	// C_EnterVehicle with no exit in front of it, and a client that sends the
	// pair the other way round would otherwise leave the old car believing it
	// still has a driver for the rest of the session.
	if (p.vehicleNetId != INVALID_NETID && p.vehicleNetId != v.netId)
		NoteExitVehicle(p, p.vehicleNetId);

	// Somebody else was already recorded at the wheel. That is a carjack, and
	// until now the session simply overwrote the name: the car changed owner
	// and the player it was taken from was never told, so their machine went
	// on believing it owned the car for the rest of the session. From there
	// the two ends disagree in both directions at once - the loser's client
	// refuses every snapshot the new owner sends, because a car it thinks is
	// its own is not a car it observes, so the car stands still on that screen
	// while it is driven away on the other; and the loser's own snapshots are
	// dropped by MayReportVehicle, so nothing it does with the car reaches
	// anybody. The record also ended up with two players in one seat, which
	// the backfill would then hand to the next joiner.
	//
	// Taken out here rather than in the fan-out so that the session is never,
	// even briefly, in that state, and so that sessiontest can hold it to it
	// without a socket.
	uint8_t displaced = INVALID_PLAYER;
	if (seat == 0 && v.driverPlayerId != INVALID_PLAYER &&
	    v.driverPlayerId != p.id) {
		if (Player *loser = FindById(v.driverPlayerId)) {
			displaced = loser->id;
			NoteExitVehicle(*loser, v.netId);
			// Their old complaint is about a car they no longer own, and the
			// next snapshot they send for it is the ordinary one-in-flight
			// race rather than a client lying. Let them earn a new warning.
			loser->warnedVehicleAuthority = false;
		} else {
			// A slot that has been freed since. RemovePeer already clears the
			// driver of every car a leaver was in, so this is belt and braces
			// against a stale id rather than a case that is reached.
			v.driverPlayerId = INVALID_PLAYER;
		}
	}

	p.vehicleNetId = v.netId;
	p.seat         = seat;

	// Only the driver's seat carries ownership. A passenger is recorded so a
	// joiner can be told where they are sitting, and gets no say over the
	// car's position or condition - see MayReportVehicle.
	if (seat == 0) {
		v.driverPlayerId = p.id;
		// A driver ends a custody (protocol.h, S_VehicleCustody). No packet
		// is spent saying so and none is needed: "the driver, or the
		// custodian when there is no driver" is one rule with a precedence in
		// it, so the S_EnterVehicle that names the driver already says this
		// as well. What must not happen is the record keeping both - a car
		// with a driver and a custodian is two machines entitled to report
		// it, which is the state protocol 22 exists to make impossible.
		v.custodianPlayerId = INVALID_PLAYER;
	}

	return displaced;
}

void Session::NoteExitVehicle(Player &p, uint16_t netId) {
	// The car stays in the session either way. Somebody parked it; it did not
	// stop existing.
	if (Vehicle *v = FindVehicle(netId))
		if (v->driverPlayerId == p.id) {
			v->driverPlayerId = INVALID_PLAYER;

			// And the machine that was driving it a moment ago is handed the
			// job of finishing whatever it was doing (protocol.h,
			// S_VehicleCustody).
			//
			// **The player who was driving, not the session host.**
			// roadmap.md §5.8 is right that an ownerless world entity is the
			// host's, and that is the right rule for a *fact* about a car
			// nobody owns - the host is one machine and it is always there.
			// It is the wrong machine to run a car's physics on: GTA III
			// streams around one player (roadmap §2.1) and keeps one island's
			// collision in memory (§2.2), so a host across the river would be
			// simulating a car with no ground under it and reporting the
			// fall. The player who has just stepped out is standing next to
			// it. That is the entire argument, and it is also why custody is
			// short: two seconds on the custodian's own clock, after which
			// the car goes back to being pinned by everybody.
			//
			// Not granted to a wreck. Its shape came from
			// FuckCarCompletely and is the same on every machine, and a
			// custodian would be asked to settle a car that has already
			// finished doing everything it is ever going to do.
			if (!v->destroyed)
				v->custodianPlayerId = p.id;
		}

	if (p.vehicleNetId == netId || netId == INVALID_NETID) {
		p.vehicleNetId = INVALID_NETID;
		p.seat         = 0;
	}
}

uint8_t Session::CustodianOf(uint16_t netId) const {
	const Vehicle *v = FindVehicle(netId);
	return v && v->active ? v->custodianPlayerId : INVALID_PLAYER;
}

// The custodian saying it is finished (protocol.h, C_VehicleSettled).
//
// Only the custodian may end its own custody, for the same reason only the
// driver may report a car's state: a message that any client could send would
// be a client deciding somebody else's ownership, which is the class of bug
// the whole of protocol 22 was about.
bool Session::EndCustody(uint16_t netId, uint8_t byPlayerId) {
	Vehicle *v = FindVehicle(netId);
	if (!v || !v->active)
		return false;
	if (v->custodianPlayerId == INVALID_PLAYER ||
	    v->custodianPlayerId != byPlayerId)
		return false;
	v->custodianPlayerId = INVALID_PLAYER;
	return true;
}

Session::HitCustody Session::CustodyForHit(uint16_t netId, uint8_t byPlayerId) {
	Vehicle *v = FindVehicle(netId);
	if (!v || !v->active || v->destroyed)
		return HitCustody::NotTheirs;
	if (v->driverPlayerId != INVALID_PLAYER)
		return HitCustody::NotTheirs;
	if (!FindById(byPlayerId))
		return HitCustody::NotTheirs;
	if (v->custodianPlayerId == byPlayerId)
		return HitCustody::AlreadyTheirs;
	if (v->custodianPlayerId != INVALID_PLAYER)
		return HitCustody::NotTheirs;
	v->custodianPlayerId = byPlayerId;
	return HitCustody::Granted;
}

// Who may report this car's position and condition.
//
// The driver, and - only when there is no driver at all - the one machine the
// session has asked to settle it. Written as a precedence rather than as two
// independent tests so that a record which somehow held both would still have
// exactly one answer: a car with a driver is a car whose custody is over,
// whatever a stale field says.
bool Session::MayReportVehicle(uint8_t playerId, uint16_t netId) const {
	const Vehicle *v = FindVehicle(netId);
	if (!v || !v->active)
		return false;
	if (v->driverPlayerId != INVALID_PLAYER)
		return v->driverPlayerId == playerId;
	return v->custodianPlayerId != INVALID_PLAYER &&
	       v->custodianPlayerId == playerId;
}

Player *Session::VehicleHitRecipient(uint16_t netId, uint8_t byPlayerId) {
	Vehicle *v = FindVehicle(netId);
	if (!v || !v->active)
		return nullptr;
	// Whose it is: MayReportVehicle's precedence, driver and then custodian.
	// The custodian's engine is the one simulating the car while it settles
	// and the one streaming its health, so it is the one to take the hit -
	// the same reason the driver is.
	const uint8_t owner = v->driverPlayerId != INVALID_PLAYER
	                          ? v->driverPlayerId
	                          : v->custodianPlayerId;
	// The inversion, and it is this function's whole reason for existing
	// separately from MayReportVehicle above. Only a machine that does *not*
	// own the car may say this.
	if (owner == byPlayerId)
		return nullptr;
	// A car nobody is driving or settling has nobody entitled to decide its
	// condition. Refused rather than routed: roadmap.md §5.8 is what carries
	// what happens to those, and it runs after the fact rather than before
	// it. That includes a hit that was fired during a settle and lands after
	// it ended - the car is nobody's by then and there is nobody to give it to.
	if (owner == INVALID_PLAYER)
		return nullptr;
	if (v->destroyed)
		return nullptr;
	// And somebody has to be there to be told. A driver or custodian who left
	// had the claim cleared with them, so this is belt and braces rather than
	// a race anyone has seen.
	return FindById(owner);
}

void Session::NoteVehicleState(const VehicleStateBody &body) {
	Vehicle *v = FindVehicle(body.netId);
	if (!v)
		return;

	// Where it is, so a later joiner spawns it here rather than back where it
	// was claimed...
	v->pos = body.pos;
	v->rot = body.rot;
	// ...and what shape it's in, so they spawn *this* car rather than a new
	// one wearing its paint.
	v->health = body.health;
	v->flags  = body.flags;

	// A car reports itself wrecked. Health alone is deliberately not the test:
	// an observer writing zero into m_fHealth produces a car that reads dead
	// and behaves new, so zero health is a number and VEH_WRECKED is a fact.
	// The driver's machine is the only one that can tell them apart.
	if (body.flags & VEH_WRECKED)
		DestroyVehicle(body.netId);
}

// What shape a car is in. docs/cardamage.md.
//
// Merged, never assigned, and the merge is what makes the arbitration cheap:
// every ladder in CDamageManager climbs and none of them descends, so a
// componentwise maximum is what the engine would have produced if one machine
// had simulated every collision. A duplicate adds nothing, a reordering
// changes nothing, and two machines that each saw half of a shunt agree on the
// union of the two halves - which is why this needs no sequence number.
//
// Returning false for "nothing new" is not an optimisation. Damage is
// near-static and absolute, so without it a client that resent its whole word
// would turn a handful of events per session into a relay loop, and every
// observer would re-run CAutomobile::SetDoorDamage - RenderWare work - for a
// door that has not moved.
bool Session::NoteVehicleDamage(const VehicleDamageBody &in,
                                VehicleDamageBody &out) {
	Vehicle *v = FindVehicle(in.netId);
	if (!v || !v->active)
		return false;

	// A wreck is finished. CDamageManager::FuckCarCompletely gave it the same
	// damage on every machine already, and a pre-blast word arriving late
	// would have the session remember a car that is less broken than the one
	// everybody is looking at.
	if (v->destroyed)
		return false;

	// A repair. The only thing in the engine that lowers a car's damage is
	// CAutomobile::Fix, which a Pay'n'Spray runs, and the car's owner says so
	// with this bit rather than with a word the join would swallow. Absolute:
	// the record goes back to nothing, so a joiner is not handed dents that
	// were sprayed off ten minutes ago.
	//
	// Relayed even when the record was already clean, because the marker is
	// also what tells every observer's row to stop believing in the dents it
	// is holding, and the server does not know what they are holding.
	if (IsDamageReset(in.panels)) {
		v->damagePanels = 0;
		v->damageDoors  = 0;
		out        = VehicleDamageBody{};
		out.netId  = v->netId;
		out.panels = VEH_DAMAGE_RESET;
		out.doors  = 0;
		return true;
	}

	const uint32_t wasPanels = v->damagePanels;
	const uint16_t wasDoors  = v->damageDoors;
	MergeDamage(v->damagePanels, v->damageDoors, in.panels, in.doors);
	if (v->damagePanels == wasPanels && v->damageDoors == wasDoors)
		return false;

	out        = VehicleDamageBody{};
	out.netId  = v->netId;
	// What the session now believes, not what one machine happened to see.
	out.panels = v->damagePanels;
	out.doors  = v->damageDoors;
	return true;
}

void Session::DestroyVehicle(uint16_t netId) {
	Vehicle *v = FindVehicle(netId);
	if (!v || v->destroyed)
		return;

	v->destroyed = true;
	v->health    = 0.0f;
	v->flags     = static_cast<uint8_t>(v->flags | VEH_WRECKED);

	// Nobody is sitting in a wreck - and nobody means the passengers too, not
	// just whoever was driving. Clearing every end of it stops the backfill
	// offering a seat in it, which would be a joiner warping a player into a
	// car that everyone else watched explode. `CAutomobile::BlowUpCar` does
	// the same thing on the machine it happened on: it flags the driver and
	// all eight passengers, not the driver alone.
	for (Player &p : m_players)
		if (p.active && p.vehicleNetId == netId)
			NoteExitVehicle(p, netId);
	v->driverPlayerId = INVALID_PLAYER;

	// And nobody is settling one either. `v->destroyed` was set above, so the
	// NoteExitVehicle calls that just ran already declined to grant it; this
	// is the case where the car was *already* being settled and then blew up
	// underneath its custodian. Nothing is left to finish, and a custodian
	// still holding it would go on being the one machine entitled to report a
	// wreck's position.
	v->custodianPlayerId = INVALID_PLAYER;
}

// ---- what a joiner is told -------------------------------------------------

S_PlayerJoin Session::MakeJoin(const Player &p, uint32_t sendTimeMs) const {
	S_PlayerJoin join;
	InitHeader(join, sendTimeMs);
	join.playerId = p.id;
	join.netId    = p.netId;

	std::memset(join.nick, 0, NICK_LEN);
	std::memcpy(join.nick, p.nick.data(), std::min(NICK_LEN - 1, p.nick.size()));

	join.modelId = p.modelId;
	join.pos     = p.pos;
	join.heading = p.heading;

	join.health      = p.health;
	join.armour      = p.armour;
	join.weapon      = p.weapon;
	join.deathAnimId = p.alive ? ANIM_NONE : p.deathAnimId;

	join.flags = 0;
	if (p.havePos)
		join.flags |= PJF_POS_VALID;
	if (!p.alive)
		join.flags |= PJF_DEAD;
	return join;
}

Backfill Session::BuildBackfill(uint8_t joinerId, uint32_t sendTimeMs) const {
	Backfill out;

	for (const Player &p : m_players) {
		if (!p.active || p.id == joinerId)
			continue;
		out.players.push_back(MakeJoin(p, sendTimeMs));

		// Their doors, if any are off their resting position right now.
		// Nothing for the overwhelmingly common case, which is why this is a
		// separate list rather than four bytes on the join packet: a garage
		// mask is zero for every player almost all of the time, and a field
		// that is always zero on a packet everybody sends is a field nobody
		// reads.
		if (p.garageMask != 0) {
			S_GarageState g{};
			g.hdr.opcode       = S_GarageState::OPCODE;
			g.hdr.sendTimeMs   = sendTimeMs;
			g.playerId         = p.id;
			g.body.deviating   = p.garageMask;
			out.garages.push_back(g);
		}
	}

	// What everyone is carrying in the slots they are not holding. Right
	// after the joins on purpose: the client has to know a player exists
	// before it can be told what is in their pockets, and CH_EVENT keeps
	// the order these are pushed in.
	if (m_ammoSync) {
		for (const Player &p : m_players) {
			if (!p.active || p.id == joinerId)
				continue;
			for (uint8_t w = 0; w < INVENTORY_SLOTS; ++w) {
				if (!p.ammoKnown[w])
					continue;
				S_PlayerAmmo ammo;
				InitHeader(ammo, sendTimeMs);
				ammo.playerId    = p.id;
				ammo.slot.weapon = w;
				ammo.slot.flags  = AMMO_SLOT_OWNED;
				ammo.slot.clip   = p.ammoClip[w];
				ammo.slot.total  = p.ammoTotal[w];
				out.ammo.push_back(ammo);
			}
		}
	}

	// The cheats every machine runs, at the state the last one left them. A
	// riot typed an hour ago is still a riot on every screen but the joiner's
	// otherwise. From nobody in particular: the typist may have left, and a
	// slot id could by now be the joiner's own, which its client would take
	// for its own cheat coming back and drop.
	for (const WorldCheat &c : m_worldCheats) {
		if (!c.set || !CheatAllowed(m_cheatRule, c.body.cheat))
			continue;
		S_Cheat cheat;
		InitHeader(cheat, sendTimeMs);
		cheat.playerId = INVALID_PLAYER;
		cheat.body     = c.body;
		out.cheats.push_back(cheat);
	}

	for (const Vehicle &v : m_vehicles) {
		if (!v.active)
			continue;

		// A wreck is left out. The people already here watched it happen;
		// the joiner is simply never told there is a car there, which is a
		// smaller lie than being told there is a working one.
		//
		// The alternative - send it with VEH_WRECKED and let the receiver
		// build a burnt-out shell - is the more faithful answer and is one
		// branch away: drop this continue and the packet below already
		// carries everything it would need. It is out because nothing on the
		// client can yet *make* a wreck (that is the vehicle seam's open
		// question), and a client that is handed VEH_WRECKED and ignores it
		// reproduces the original bug exactly.
		if (v.destroyed)
			continue;

		S_VehicleSpawn spawn;
		InitHeader(spawn, sendTimeMs);
		spawn.netId   = v.netId;
		spawn.modelId = v.modelId;
		spawn.pos     = v.pos;
		spawn.rot     = v.rot;
		spawn.colour1 = v.colour1;
		spawn.colour2 = v.colour2;
		spawn.extra1  = v.extra1;
		spawn.extra2  = v.extra2;
		spawn.health  = v.health;
		spawn.flags   = v.flags;
		out.vehicles.push_back(spawn);

		// ...and what it has been through. Only when there is something to
		// say, because most cars in a session have never been touched and a
		// packet that carries two zeroes is a packet nobody can read a
		// capture of.
		if (v.damagePanels != 0 || v.damageDoors != 0) {
			S_VehicleDamage dmg{};
			InitHeader(dmg, sendTimeMs);
			dmg.playerId    = INVALID_PLAYER;   // the session's record, not a report
			dmg.body.netId  = v.netId;
			dmg.body.panels = v.damagePanels;
			dmg.body.doors  = v.damageDoors;
			out.vehicleDamage.push_back(dmg);
		}
	}

	// And who is sitting in which car, driver or passenger. The spawn above
	// puts the car on screen, this puts the people inside it; without both,
	// the joiner sees the car driving itself while its occupants jog along on
	// the roof.
	for (const Player &p : m_players) {
		if (!p.active || p.id == joinerId || p.vehicleNetId == INVALID_NETID)
			continue;
		// A dead player is in no seat. Their own client took them out of it
		// before killing the ped, and NotePlayerDied made the session agree -
		// this is belt and braces for the case where a death arrives while
		// the enter is still being processed.
		if (!p.alive)
			continue;

		const Vehicle *v = FindVehicle(p.vehicleNetId);
		if (!v || v->destroyed)
			continue;   // the car was never sent, so there is no seat to take

		S_EnterVehicle seat;
		InitHeader(seat, sendTimeMs);
		seat.playerId     = p.id;
		seat.body.netId   = v->netId;
		// The seat they are actually in. Sending 0 here - which is what this
		// did before, because the session only ever wrote down drivers - told
		// the joiner that a passenger was behind the wheel, so the joiner was
		// the one machine in the session with somebody driving a car that
		// everyone else could see them being a passenger in.
		seat.body.seat    = p.seat;
		seat.body.modelId = v->modelId;
		seat.body.colour1 = v->colour1;
		seat.body.colour2 = v->colour2;
		seat.body.pos     = v->pos;
		seat.body.rot     = v->rot;
		out.seats.push_back(seat);
	}

	// The city the joiner is walking into. Their own engine is about to
	// generate its own crowd on top of this, which is the doubling
	// docs/population.md §1.3 fixes and this slice does not - see the note
	// on Session::AddPed.
	//
	// tempId is 0 on every one of these, and that is load-bearing rather than
	// tidy: it is the value no creator ever allocates, so a joiner reading
	// its own ownerPlayerId off one of these - which cannot happen, since the
	// joiner owns nothing yet - still could not mistake it for an answer to a
	// claim.
	for (const AmbientPed &ped : m_peds) {
		if (!ped.active || ped.ownerPlayerId == joinerId)
			continue;

		S_PedSpawn spawn;
		InitHeader(spawn, sendTimeMs);
		spawn.ownerPlayerId = ped.ownerPlayerId;
		spawn.tempId        = 0;
		spawn.netId         = ped.netId;
		spawn.body          = ped.body;
		out.peds.push_back(spawn);

		// A corpse. Unlike a wrecked car, which is left out of the backfill
		// entirely because S_CarSpawn cannot describe one, a dead pedestrian
		// is spawned and then killed: S_PedDeath exists, the joiner's engine
		// is the thing that decides what a death looks like, and the
		// alternative - leaving him out - is a body everybody else can see
		// and walk around that the joiner walks straight through.
		if (ped.alive)
			continue;
		S_PedDeath death;
		InitHeader(death, sendTimeMs);
		death.body.netId  = ped.netId;
		death.body.animId = ped.deathAnimId;
		out.pedDeaths.push_back(death);
	}

	// The traffic, beside the pedestrians. Each carries the position the
	// session last heard on the owner's stream rather than the one the car
	// was created at - a traffic car that was born two minutes ago is nowhere
	// near where it was born, and handing a joiner the birth position puts a
	// car across a junction it left long since.
	for (const AmbientCar &car : m_cars) {
		if (!car.active || car.ownerPlayerId == joinerId)
			continue;
		// A wreck is left out, exactly as a wrecked session car is
		// (docs/protocol.md §2.8.5): S_CarSpawn has no way to say "burnt
		// shell", and its host will stop streaming the thing within the
		// minute the engine takes to reap it.
		if (car.destroyed)
			continue;

		S_CarSpawn spawn;
		InitHeader(spawn, sendTimeMs);
		spawn.ownerPlayerId = car.ownerPlayerId;
		spawn.tempId        = 0;
		spawn.netId         = car.netId;
		spawn.body          = car.body;
		out.cars.push_back(spawn);
	}

	// Which pickups are currently gone, so a joiner is not the one player in
	// the session who can see a hidden package everybody else has collected.
	// Last, because the removal it drives is unconditional and nothing else
	// in the backfill depends on it.
	for (const TakenPickup &t : m_pickups) {
		// Reservations are not told to a joiner. Nothing has been picked up,
		// so there is nothing for them to remove - and if the holder walks
		// away the reservation simply ends.
		if (t.state != TakenPickup::State::TAKEN)
			continue;
		S_PickupTaken taken;
		InitHeader(taken, sendTimeMs);
		taken.playerId = t.byPlayerId;
		taken.ident    = t.ident;
		out.pickups.push_back(taken);
	}

	// And which cars nobody owns are already wrecks. Beside the pickups for
	// the same reason they are beside each other in the design: both are
	// "things in the world that are gone, that the joiner's own engine will
	// otherwise happily put back".
	for (const WreckedUnownedCar &w : m_unownedWrecks) {
		// Zero-initialised, which matters now that the packet carries a
		// transform: only UNOWNED_PARKED is ever in this table, that kind
		// never reads one, and a backfill that put stale stack bytes on the
		// wire would be a packet nobody could read a capture of.
		S_UnownedBlowUp blast{};
		InitHeader(blast, sendTimeMs);
		blast.reporterPlayerId = w.byPlayer;
		blast.key              = w.key;
		out.unownedWrecks.push_back(blast);
	}

	return out;
}

// ---- cars nobody owns ------------------------------------------------------

bool Session::NoteUnownedBlowUp(const UnownedVehicleKey &key, uint8_t byPlayerId,
                                uint32_t nowMs) {
	// A kind this build does not speak is dropped rather than recorded. The
	// alternative - store it and relay it - would have the server handing
	// clients a key they cannot resolve, which is a packet that only ever
	// produces a log line.
	//
	// A car the session does have a row for, parked and walked away from.
	// This one does not go in the wreck table at all: Vehicle::destroyed is
	// already the record, DestroyVehicle is already the single place it is
	// written, and a destroyed car is already left out of the backfill. A
	// second record would be a second answer to the same question.
	if (key.kind == UNOWNED_SESSION) {
		const Vehicle *v = FindVehicle(key.id);
		if (!v || !v->active || v->destroyed)
			return false;
		// With a driver it is the driver's, and C_VehicleBlowUp is how they
		// say so - a driver is the only one whose physics decided where the
		// car ended up. Without one it belongs to nobody and whoever was
		// standing next to it may report it.
		if (v->driverPlayerId != INVALID_PLAYER)
			return false;
		// Unless somebody is settling it. Then the custodian's engine is the
		// only one simulating it, every other client refuses to blow it up,
		// and a report from one of them is a copy that went its own way.
		// The custodian says it with this packet, since it isn't driving.
		if (v->custodianPlayerId != INVALID_PLAYER &&
		    v->custodianPlayerId != byPlayerId)
			return false;
		DestroyVehicle(key.id);
		return true;
	}

	// A traffic car one machine's engine made, hosts and has now destroyed.
	//
	// Not in the wreck table either, and for the same reason the session kind
	// is not: the AmbientCar row is already the record, and it already
	// decides what a joiner is told. What is different is who may say it.
	// Ambient traffic has no driver and never will, so "the driver decides"
	// has no meaning here - the rule that replaces it is that the car belongs
	// to the machine whose engine created it, and only that machine may
	// report its death. Anybody else reporting it is an observer deciding
	// that somebody else's car died because its own replica blew up locally,
	// which is the one thing the replicas must never be allowed to do.
	if (key.kind == UNOWNED_AMBIENT) {
		AmbientCar *car = FindCar(key.id);
		if (!car || !car->active || car->destroyed)
			return false;
		if (car->ownerPlayerId != byPlayerId)
			return false;
		car->destroyed = true;
		return true;
	}

	if (key.kind != UNOWNED_PARKED)
		return false;

	for (const WreckedUnownedCar &w : m_unownedWrecks)
		if (SameUnownedKey(w.key, key))
			return false;   // somebody already said so; not a contest

	WreckedUnownedCar rec;
	rec.key      = key;
	rec.byPlayer = byPlayerId;
	rec.atMs     = nowMs;
	m_unownedWrecks.push_back(rec);
	return true;
}

void Session::ExpireUnownedWrecks(uint32_t nowMs) {
	for (size_t i = 0; i < m_unownedWrecks.size();) {
		if (nowMs - m_unownedWrecks[i].atMs >= WRECK_BACKFILL_MS) {
			m_unownedWrecks[i] = m_unownedWrecks.back();
			m_unownedWrecks.pop_back();
			continue;
		}
		++i;
	}
}

// ---- pickups ---------------------------------------------------------------

bool Session::SameIdent(const PickupIdent &a, const PickupIdent &b) {
	if (a.modelIndex != b.modelIndex)
		return false;
	const float dx = a.pos.x - b.pos.x;
	const float dy = a.pos.y - b.pos.y;
	const float dz = a.pos.z - b.pos.z;
	// 0.25 m, the same tolerance client/src/game/pickup.h uses. A literal on
	// both sides rather than a shared constant, because the client's copy is
	// documented against the script coordinates it was measured from and this
	// one is not.
	return dx * dx + dy * dy + dz * dz <= 0.25f * 0.25f;
}

TakenPickup *Session::FindPickup(const PickupIdent &ident) {
	for (TakenPickup &t : m_pickups)
		if (SameIdent(t.ident, ident))
			return &t;
	return nullptr;
}

Session::PickupVerdict Session::ClaimPickup(uint8_t playerId,
                                            const PickupIdent &ident,
                                            uint32_t nowMs) {
	if (TakenPickup *held = FindPickup(ident)) {
		// Unsigned subtraction throughout, so a server that has been up past
		// 2^32 ms reads as a small elapsed time rather than an enormous one -
		// the same rule KillCreditFor follows on the client.
		if (held->state == TakenPickup::State::RESERVED) {
			// Somebody is standing on it. Re-granting to the same player is
			// harmless and keeps a re-sent claim from being refused, but the
			// reservation still belongs to whoever has it.
			if (held->byPlayerId != playerId &&
			    nowMs - held->sinceMs < PICKUP_RESERVATION_MS)
				return PickupVerdict::DENIED;
		} else {
			// Never comes back, and nobody has released it: it is gone.
			if (held->respawnMs == 0)
				return PickupVerdict::DENIED;
			if (nowMs - held->sinceMs < held->respawnMs)
				return PickupVerdict::DENIED;
		}

		// Free, one way or the other. Reuse the record rather than growing
		// the vector: a shop counter is claimable every five seconds for as
		// long as anybody stands near it.
		held->ident      = ident;
		held->state      = TakenPickup::State::RESERVED;
		held->byPlayerId = playerId;
		held->sinceMs    = nowMs;
		held->respawnMs  = 0;
		return PickupVerdict::GRANTED;
	}

	TakenPickup t;
	t.ident      = ident;
	t.state      = TakenPickup::State::RESERVED;
	t.byPlayerId = playerId;
	t.sinceMs    = nowMs;
	m_pickups.push_back(t);
	return PickupVerdict::GRANTED;
}

bool Session::NotePickupCollected(uint8_t playerId, const PickupIdent &ident,
                                  uint32_t nowMs) {
	TakenPickup *held = FindPickup(ident);
	if (!held || held->byPlayerId != playerId ||
	    held->state != TakenPickup::State::RESERVED)
		return false;

	held->state     = TakenPickup::State::TAKEN;
	held->sinceMs   = nowMs;
	held->respawnMs =
	    PickupRespawnMs(ident.type, (ident.flags & PICKUP_F_BRIBE) != 0);
	return true;
}

void Session::ReleaseReservationsOf(uint8_t playerId) {
	for (size_t i = 0; i < m_pickups.size();) {
		if (m_pickups[i].state == TakenPickup::State::RESERVED &&
		    m_pickups[i].byPlayerId == playerId) {
			m_pickups[i] = m_pickups.back();
			m_pickups.pop_back();
			continue;
		}
		++i;
	}
}

void Session::ReleasePickup(const PickupIdent &ident) {
	for (size_t i = 0; i < m_pickups.size(); ++i) {
		if (!SameIdent(m_pickups[i].ident, ident))
			continue;
		m_pickups[i] = m_pickups.back();
		m_pickups.pop_back();
		return;
	}
}

void Session::ExpirePickups(uint32_t nowMs) {
	for (size_t i = 0; i < m_pickups.size();) {
		const TakenPickup &t = m_pickups[i];

		const bool gone =
		    t.state == TakenPickup::State::RESERVED
		        // A holder who never said anything again: a crashed client,
		        // or a connection lost between the grant and the release.
		        ? nowMs - t.sinceMs >= PICKUP_RESERVATION_MS
		        // respawnMs == 0 is "never", and never does not expire. Those
		        // are the ONCE / COLLECTABLE1 / MONEY pickups, and they are
		        // the ones a joiner most needs telling about - a hidden
		        // package the server forgot would count twice for the group.
		        : t.respawnMs != 0 && nowMs - t.sinceMs >= t.respawnMs;

		if (gone) {
			m_pickups[i] = m_pickups.back();
			m_pickups.pop_back();
			continue;
		}
		++i;
	}
}

// ---------------------------------------------------------------------------
// The session's rampage - docs/roadmap.md 5.10
// ---------------------------------------------------------------------------

bool Session::NoteRampageStart(uint8_t byPlayer, const RampageStartBody &in,
                               uint32_t nowMs, RampageOpenBody &out) {
	if (m_rampageRule == RAMPAGE_RULE_OFF)
		return false;

	if (!m_rampage.open) {
		m_rampage            = Rampage{};
		m_rampage.open       = true;
		m_rampage.id         = m_nextFrenzyId++;
		// Never zero, because a frenzy id of zero would be indistinguishable
		// from an uninitialised field on the client.
		if (m_nextFrenzyId == 0)
			m_nextFrenzyId = 1;
		m_rampage.limitMs    = in.limitMs;
		m_rampage.openedAtMs = nowMs;
		m_rampage.openedBy   = byPlayer;
		// The one decision the server makes here, and the only place it can
		// be made: nothing on a client knows how many players there are at
		// the instant the script asks, and two clients that worked it out for
		// themselves would disagree the moment somebody was mid-join.
		//
		// Count(), not m_players.size(): the vector is pre-sized to the
		// session's slots and every one of them is a Player whether anybody
		// is in it or not, so size() is 8 in an empty session and a
		// two-player rampage would be asked for eight times the kills.
		m_rampage.target = ScaledRampageTarget(in.target, m_rampageRule, Count());
	}

	out.frenzyId = m_rampage.id;
	// What is still wanted, not what was asked for. For the machine that
	// opened it those are the same number; for a machine that has just walked
	// into a rampage the group is halfway through, they are not.
	out.killsNeeded = m_rampage.kills >= m_rampage.target
	                      ? uint16_t(0)
	                      : static_cast<uint16_t>(m_rampage.target - m_rampage.kills);
	out.elapsedMs   = nowMs - m_rampage.openedAtMs;
	return true;
}

bool Session::NoteRampageKill(const RampageKillBody &in) {
	if (!m_rampage.open || in.frenzyId != m_rampage.id)
		return false;
	// Counted but never used to end anything. The server does not decide that
	// a rampage has been passed: the clients' own CDarkel::Update does, off a
	// counter every one of them is now driving with the same events, and one
	// of them then reports it. All this number is for is telling a joiner how
	// many are left.
	if (m_rampage.kills < 0xFFFF)
		++m_rampage.kills;
	return true;
}

bool Session::NoteRampageCar(const RampageCarBody &in) {
	if (!m_rampage.open || in.frenzyId != m_rampage.id)
		return false;

	if (RampageCarKeyed(in.key)) {
		for (uint8_t i = 0; i < m_rampage.carKeyCount; ++i)
			if (SameUnownedKey(m_rampage.carKeys[i], in.key))
				return false;   // somebody else's copy of it already counted
		m_rampage.carKeys[m_rampage.carKeyNext] = in.key;
		m_rampage.carKeyNext =
		    static_cast<uint8_t>((m_rampage.carKeyNext + 1) % RAMPAGE_CAR_KEYS);
		if (m_rampage.carKeyCount < RAMPAGE_CAR_KEYS)
			++m_rampage.carKeyCount;
	}

	// Same counter as the kills. A vehicle rampage only ever gets cars and a
	// pedestrian one only ever gets people, because each machine's engine
	// judged it before sending, so one number is enough for a joiner.
	if (m_rampage.kills < 0xFFFF)
		++m_rampage.kills;
	return true;
}

bool Session::NoteRampageEnd(const RampageEndBody &in, RampageEndBody &out) {
	if (!m_rampage.open || in.frenzyId != m_rampage.id)
		return false;
	if (!IsRampageOutcome(in.outcome))
		return false;

	// First report wins. The others are the rest of the session reaching the
	// same conclusion a few milliseconds later - or, in the one case this
	// exists for, reaching the opposite one: a machine whose clock ran out
	// while the winning kill was still on the wire.
	m_rampage.open = false;
	out.frenzyId   = in.frenzyId;
	out.outcome    = in.outcome;
	return true;
}

bool Session::ExpireRampage(uint32_t nowMs, RampageEndBody &out) {
	if (!m_rampage.open)
		return false;
	// A rampage the script gave no time limit never times out, here or in the
	// engine (`cmp dword [00885BACh],0 / jl` at 0x00420696).
	if (m_rampage.limitMs < 0)
		return false;

	const uint32_t deadline =
	    static_cast<uint32_t>(m_rampage.limitMs) + RAMPAGE_GRACE_MS;
	if (nowMs - m_rampage.openedAtMs < deadline)
		return false;

	m_rampage.open = false;
	out.frenzyId   = m_rampage.id;
	out.outcome    = RAMPAGE_FAILED;
	return true;
}


// The host keeps the job until they leave, and then the lowest active slot
// takes it.
//
// The "still here?" test in front is not a shortcut, it is the whole rule.
// Slots are reused, so picking the lowest active slot unconditionally would
// hand the session's clock to whoever happened to fill slot 0 next - a player
// who just connected, whose game is at whatever time they left it, and who
// would then drag everyone else's clock to it. A handover should only ever
// happen because the host left.
void Session::PickHost() {
	if (m_hostId != INVALID_PLAYER && FindById(m_hostId))
		return;

	m_hostId = INVALID_PLAYER;
	for (const Player &p : m_players)
		if (p.active) {
			m_hostId = p.id;
			return;
		}

	// Nobody left. Whatever the cheats did lives on in engines that are no
	// longer here, and the next player to arrive has not had any of it.
	for (WorldCheat &c : m_worldCheats)
		c = WorldCheat{};
	// The wallet too. Whoever comes back first brings it with him - he
	// adopted it - and a stranger's save isn't told to spend it.
	ClearMoney();
}

// ---- money -------------------------------------------------------------------

void Session::ClearMoney() {
	m_moneySeeded = false;
	m_moneyPool   = 0;
	for (PaidCar &c : m_paidCars)
		c = PaidCar{};
	m_paidCarNext = 0;
}

void Session::SetMoneyRule(uint8_t rule) {
	rule = SaneMoneyRule(rule);
	if (rule == m_moneyRule)
		return;
	m_moneyRule = rule;
	ClearMoney();
}

bool Session::NoteMoneyChange(uint8_t from, const MoneyChangeBody &body) {
	if (m_moneyRule != MONEY_RULE_SHARED)
		return false;
	Player *p = FindById(from);
	if (!p)
		return false;
	// CH_EVENT is ordered, so this is only ever a client repeating itself.
	if (body.seq == 0 || body.seq <= p->moneySeq)
		return false;
	p->moneySeq = body.seq;

	if (!m_moneySeeded) {
		m_moneySeeded = true;
		m_moneyPool   = AddToMoneyPool(0, body.have);
		return true;
	}
	// Even a zero goes out: it is a joiner who tried to seed a pool somebody
	// else had seeded first, and he needs the total to adopt.
	m_moneyPool = AddToMoneyPool(m_moneyPool, body.delta);
	return true;
}

S_Money Session::MoneyFor(uint8_t to, uint8_t from, int32_t delta,
                          uint32_t sendTimeMs) const {
	S_Money out;
	InitHeader(out, sendTimeMs);
	const bool pooled = m_moneyRule == MONEY_RULE_SHARED && m_moneySeeded;
	out.rule         = m_moneyRule;
	out.flags        = pooled ? uint8_t(MONEY_POOL_SEEDED) : uint8_t(0);
	out.fromPlayerId = from;
	out.pad          = 0;
	out.total        = pooled ? m_moneyPool : 0;
	out.ackSeq       = 0;
	for (const Player &p : m_players)
		if (p.active && p.id == to)
			out.ackSeq = p.moneySeq;
	out.delta = delta;
	return out;
}

bool Session::TakeMoneyAward(uint8_t from, const MoneyAwardBody &body, uint32_t nowMs) {
	if (m_moneyRule == MONEY_RULE_OFF)
		return false;
	if (!FindById(from) || !FindById(body.toPlayerId))
		return false;
	if (!IsSaneMoneyAward(body.unit) || body.kind > MONEY_AWARD_BOMB)
		return false;
	if (!MoneyAwardKeyed(body.key))
		return true;

	for (const PaidCar &c : m_paidCars)
		if (c.used && SameUnownedKey(c.key, body.key) &&
		    nowMs - c.paidMs < MONEY_AWARD_KEY_MS)
			return false;
	PaidCar &slot = m_paidCars[m_paidCarNext];
	slot.key      = body.key;
	slot.paidMs   = nowMs;
	slot.used     = true;
	m_paidCarNext = (m_paidCarNext + 1) % PAID_CARS;
	return true;
}

uint8_t Session::NoteCheat(uint8_t from, const CheatBody &body) {
	const uint8_t relay = CheatRelayFor(m_cheatRule, body.cheat, body.state,
	                                    from == m_hostId, m_hostId != INVALID_PLAYER);
	if (relay == CHEAT_RELAY_OTHERS) {
		// TIMEFLIES and BOOOOORING are one setting reached from two ends, so
		// they share a record and the later one wins.
		const uint8_t slot = body.cheat == CHEAT_SLOW_TIME ? uint8_t(CHEAT_FAST_TIME)
		                                                    : body.cheat;
		m_worldCheats[slot].set  = true;
		m_worldCheats[slot].body = body;
	}
	return relay;
}

bool Session::SetWeather(uint8_t weather, uint8_t weatherOld) {
	// eWeatherType is 0..3 and CWeather indexes arrays with it without a
	// bounds check, so a bad value would be a crash on every client rather
	// than a wrong sky on one.
	constexpr uint8_t WEATHER_TOTAL = 4;
	if (weather >= WEATHER_TOTAL || weatherOld >= WEATHER_TOTAL)
		return false;
	m_weather    = weather;
	m_weatherOld = weatherOld;
	return true;
}

Player *Session::FindByPeer(uint32_t peer) {
	auto it = std::find_if(m_players.begin(), m_players.end(), [peer](const Player &p) {
		return p.active && p.peer == peer;
	});
	return it == m_players.end() ? nullptr : &*it;
}

Player *Session::FindByNetId(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	auto it = std::find_if(m_players.begin(), m_players.end(), [netId](const Player &p) {
		return p.active && p.netId == netId;
	});
	return it == m_players.end() ? nullptr : &*it;
}

Player *Session::FindById(uint8_t id) {
	if (id >= m_players.size() || !m_players[id].active)
		return nullptr;
	return &m_players[id];
}

uint8_t Session::Count() const {
	return static_cast<uint8_t>(
	    std::count_if(m_players.begin(), m_players.end(),
	                  [](const Player &p) { return p.active; }));
}

Vehicle *Session::FindVehicle(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (Vehicle &v : m_vehicles)
		if (v.active && v.netId == netId)
			return &v;
	return nullptr;
}

const Vehicle *Session::FindVehicle(uint16_t netId) const {
	return const_cast<Session *>(this)->FindVehicle(netId);
}

Vehicle *Session::AddVehicle(uint16_t modelId, uint8_t colour1, uint8_t colour2,
                             const Vec3 &pos, const Quat &rot) {
	// Capped because this whole list gets replayed to every joining player,
	// and because every client holds the same number of rows. It counts cars
	// alive right now: ReleaseIdleVehicles frees the rows nobody needs, and a
	// freed row is reused here before the vector grows. It used to be reused
	// only once the vector was full, which never happened because nothing
	// ever freed one, so the 65th claim of a session was refused.
	Vehicle *slot = nullptr;
	for (Vehicle &v : m_vehicles)
		if (!v.active) {
			slot = &v;
			break;
		}
	if (!slot) {
		if (m_vehicles.size() >= MAX_SESSION_VEHICLES)
			return nullptr;
		m_vehicles.push_back(Vehicle{});
		slot = &m_vehicles.back();
	}

	*slot         = Vehicle{};
	slot->active  = true;
	slot->netId   = AllocNetId();
	slot->modelId = modelId;
	slot->colour1 = colour1;
	slot->colour2 = colour2;
	slot->pos     = pos;
	slot->rot     = rot;
	return slot;
}

size_t Session::LiveVehicleCount() const {
	return static_cast<size_t>(
	    std::count_if(m_vehicles.begin(), m_vehicles.end(),
	                  [](const Vehicle &v) { return v.active; }));
}

bool Session::VehicleNeeded(const Vehicle &v) const {
	if (!v.active)
		return false;
	if (v.driverPlayerId != INVALID_PLAYER || v.custodianPlayerId != INVALID_PLAYER)
		return true;
	constexpr float r2 = VEHICLE_KEEP_RADIUS_M * VEHICLE_KEEP_RADIUS_M;
	for (const Player &p : m_players) {
		if (!p.active)
			continue;
		// Any seat. A passenger has no say over the car but he is sitting in
		// it, and releasing it would destroy it around him on every machine.
		if (p.vehicleNetId == v.netId)
			return true;
		// 2D, like PossiblyRemoveVehicle's own distance. A player we have no
		// position for yet can't be near anything.
		if (!p.havePos)
			continue;
		const float dx = p.pos.x - v.pos.x;
		const float dy = p.pos.y - v.pos.y;
		if (dx * dx + dy * dy <= r2)
			return true;
	}
	return false;
}

std::vector<uint16_t> Session::ReleaseIdleVehicles(uint32_t nowMs) {
	std::vector<uint16_t> released;
	for (Vehicle &v : m_vehicles) {
		if (!v.active)
			continue;
		if (!v.neededKnown || VehicleNeeded(v)) {
			v.neededKnown = true;
			v.neededAtMs  = nowMs;
			continue;
		}
		// Unsigned, so a clock that wraps still measures forwards.
		if (nowMs - v.neededAtMs < VEHICLE_RELEASE_MS)
			continue;
		// A wreck goes the same way. A copy is locked, so PossiblyRemoveVehicle
		// skips its distance removal (the bIsLocked test at 0x00418448); this
		// is what takes it off every machine, and nobody is near to see it go.
		released.push_back(v.netId);
		v = Vehicle{};
	}
	return released;
}

// A traffic car becomes a session car, keeping its number. protocol.h,
// S_CarPromoted.
//
// Deliberately not AddVehicle plus RemoveCar. AddVehicle allocates a netId,
// and a new number is exactly what must not happen here: it is the number
// every machine already has this CVehicle filed under, and keeping it is what
// lets them all move their bookkeeping instead of destroying and rebuilding
// the object. Allocating a second one would put the session back in the state
// roadmap §5.8.1 case 2 describes - two netIds for one car, one following the
// driver and one frozen.
//
// The ambient row goes away here rather than being left to the host's own
// despawn. Its owner has been told to stop hosting the car, so no C_CarDespawn
// is coming for it, and a row left behind would keep answering NoteCarState
// for a netId that now belongs to something else.
Vehicle *Session::PromoteCar(uint16_t netId, uint8_t driverPlayerId,
                             uint8_t &wasOwner, AmbientCarBody &body) {
	wasOwner = INVALID_PLAYER;

	AmbientCar *car = FindCar(netId);
	if (!car || !car->active)
		return nullptr;
	// A burnt shell is not a car anybody is driving away. Its host has
	// already reported it destroyed and the roster is holding the row only so
	// the number stays spoken for.
	if (car->destroyed)
		return nullptr;
	// And the number must not already name a session car. It cannot, because
	// AllocNetId hands each number out once - this is the assertion that says
	// so rather than a case that is reached.
	if (FindVehicle(netId))
		return nullptr;

	wasOwner = car->ownerPlayerId;
	body     = car->body;

	Vehicle promoted;
	promoted.active  = true;
	promoted.netId   = netId;
	promoted.modelId = body.modelId;
	promoted.colour1 = body.colour1;
	promoted.colour2 = body.colour2;
	promoted.extra1  = body.extra1;
	promoted.extra2  = body.extra2;
	promoted.pos     = body.pos;
	promoted.rot     = body.rot;
	// Straight into the driver's seat. The claim that got here is a
	// C_EnterVehicle for seat 0 and the caller records the seat through
	// NoteEnterVehicle immediately afterwards; this is only the row existing
	// in time for it to be recorded against.

	// Somewhere to put it first, and only then take the ambient row away. The
	// other order loses the car outright on a full vehicle table: the traffic
	// roster would have forgotten it, the vehicle roster would never have had
	// it, and its host has no reason to announce it a second time.
	Vehicle *slot = nullptr;
	for (Vehicle &v : m_vehicles)
		if (!v.active) {
			slot = &v;
			break;
		}
	if (!slot) {
		if (m_vehicles.size() >= MAX_SESSION_VEHICLES) {
			wasOwner = INVALID_PLAYER;
			return nullptr;
		}
		m_vehicles.push_back(Vehicle{});
		slot = &m_vehicles.back();
	}

	*slot = promoted;
	*car  = AmbientCar{};
	return slot;
}

// ---- ambient peds ----------------------------------------------------------

AmbientPed *Session::FindPed(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (AmbientPed &ped : m_peds)
		if (ped.active && ped.netId == netId)
			return &ped;
	return nullptr;
}

const AmbientPed *Session::FindPed(uint16_t netId) const {
	return const_cast<Session *>(this)->FindPed(netId);
}

AmbientPed *Session::AddPed(uint8_t ownerPlayerId, const AmbientPedBody &body) {
	// Reuse a dead row before growing. Unlike vehicles, ambient peds come and
	// go constantly - the engine reaps one the moment its owner walks away
	// from it - so without this the vector would grow for the length of the
	// session and never shrink.
	for (AmbientPed &ped : m_peds) {
		if (ped.active)
			continue;
		ped        = AmbientPed{};
		ped.active = true;
		ped.netId  = AllocNetId();
		ped.ownerPlayerId = ownerPlayerId;
		ped.body   = body;
		return &ped;
	}

	if (m_peds.size() >= MAX_AMBIENT_PEDS)
		return nullptr;

	AmbientPed ped;
	ped.active = true;
	ped.netId  = AllocNetId();
	ped.ownerPlayerId = ownerPlayerId;
	ped.body   = body;
	m_peds.push_back(ped);
	return &m_peds.back();
}

bool Session::RemovePed(uint16_t netId, uint8_t byPlayerId) {
	AmbientPed *ped = FindPed(netId);
	if (!ped)
		return false;
	// Host-authoritative, and this is where that rule is actually enforced
	// for ambient entities: an observer that lost its replica is not
	// reporting a fact about the ped, it is reporting a fact about itself.
	if (byPlayerId != INVALID_PLAYER && byPlayerId != ped->ownerPlayerId)
		return false;

	// The row is cleared rather than erased, so the netId stays spoken for
	// until AllocNetId has moved well past it - same reasoning as a destroyed
	// Vehicle keeping its row.
	*ped = AmbientPed{};
	return true;
}

bool Session::NotePedState(const AmbientPedState &state, uint8_t byPlayerId) {
	AmbientPed *ped = FindPed(state.netId);
	if (!ped)
		return false;
	if (byPlayerId != ped->ownerPlayerId)
		return false;
	ped->body.pos     = state.pos;
	ped->body.heading = state.heading;
	return true;
}

bool Session::NotePedDeath(const PedDeathBody &death, uint8_t byPlayerId) {
	AmbientPed *ped = FindPed(death.netId);
	if (!ped)
		return false;
	if (byPlayerId != ped->ownerPlayerId)
		return false;
	// Once per life. The row is cleared and rebuilt by AddPed when the netId
	// is reused, so `alive` can only be false for a ped that really has been
	// reported dead already.
	if (!ped->alive)
		return false;
	ped->alive       = false;
	ped->deathAnimId = death.animId;
	return true;
}

Player *Session::PedDamageRecipient(uint16_t pedNetId, uint8_t byPlayerId) {
	const AmbientPed *ped = FindPed(pedNetId);
	if (!ped)
		return nullptr;
	// The inversion, and it is the whole of this function's reason for existing
	// separately from the three above. Only a machine that is *not* the owner
	// may say this.
	if (ped->ownerPlayerId == byPlayerId)
		return nullptr;
	if (!ped->alive)
		return nullptr;
	// And somebody has to be there to be told. An owner who left took their
	// pedestrians with them (Server::DropPedsOf), so this is belt and braces
	// rather than a race anyone has seen.
	return FindById(ped->ownerPlayerId);
}

std::vector<uint16_t> Session::PedsOwnedBy(uint8_t playerId) const {
	std::vector<uint16_t> out;
	if (playerId == INVALID_PLAYER)
		return out;
	for (const AmbientPed &ped : m_peds)
		if (ped.active && ped.ownerPlayerId == playerId)
			out.push_back(ped.netId);
	return out;
}

// ---- ambient traffic -------------------------------------------------------
//
// The ped block above with the names changed, plus NoteCarState. Written out
// rather than templated over the two row types: the shapes agree today and
// there is no reason they have to, and a template here would hide the one
// place they already differ.

AmbientCar *Session::FindCar(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (AmbientCar &car : m_cars)
		if (car.active && car.netId == netId)
			return &car;
	return nullptr;
}

const AmbientCar *Session::FindCar(uint16_t netId) const {
	return const_cast<Session *>(this)->FindCar(netId);
}

AmbientCar *Session::AddCar(uint8_t ownerPlayerId, const AmbientCarBody &body) {
	// Dead row first, same as AddPed: traffic is generated and recycled for as
	// long as the session runs, so growing on every car would grow forever.
	for (AmbientCar &car : m_cars) {
		if (car.active)
			continue;
		car        = AmbientCar{};
		car.active = true;
		car.netId  = AllocNetId();
		car.ownerPlayerId = ownerPlayerId;
		car.body   = body;
		return &car;
	}

	if (m_cars.size() >= MAX_AMBIENT_CARS)
		return nullptr;

	AmbientCar car;
	car.active = true;
	car.netId  = AllocNetId();
	car.ownerPlayerId = ownerPlayerId;
	car.body   = body;
	m_cars.push_back(car);
	return &m_cars.back();
}

bool Session::RemoveCar(uint16_t netId, uint8_t byPlayerId) {
	AmbientCar *car = FindCar(netId);
	if (!car)
		return false;
	if (byPlayerId != INVALID_PLAYER && byPlayerId != car->ownerPlayerId)
		return false;
	*car = AmbientCar{};
	return true;
}

std::vector<uint16_t> Session::CarsOwnedBy(uint8_t playerId) const {
	std::vector<uint16_t> out;
	if (playerId == INVALID_PLAYER)
		return out;
	for (const AmbientCar &car : m_cars)
		if (car.active && car.ownerPlayerId == playerId)
			out.push_back(car.netId);
	return out;
}

bool Session::NoteCarState(const AmbientCarState &state, uint8_t byPlayerId) {
	AmbientCar *car = FindCar(state.netId);
	if (!car)
		return false;
	if (byPlayerId != car->ownerPlayerId)
		return false;
	car->body.pos = state.pos;
	car->body.rot = state.rot;
	return true;
}

Player *Session::CarHitRecipient(uint16_t netId, uint8_t byPlayerId) {
	const AmbientCar *car = FindCar(netId);
	if (!car || car->destroyed)
		return nullptr;
	if (car->ownerPlayerId == byPlayerId)
		return nullptr;
	return FindById(car->ownerPlayerId);
}

// ---- police helicopters ------------------------------------------------------

bool Session::HeliGoneAlready(uint8_t owner, uint16_t serial) const {
	const OwnerHelis &o = m_helis[owner];
	for (uint8_t i = 0; i < o.goneCount; ++i)
		if (o.gone[i] == serial)
			return true;
	return false;
}

bool Session::NoteHeliState(uint8_t owner, const HeliStateBody &body) {
	if (owner >= MAX_PLAYERS || !IsPoliceHeliSlot(body.slot) ||
	    !IsKnownHeliStatus(body.status))
		return false;
	if (HeliGoneAlready(owner, body.serial))
		return false;
	// Overwrites whatever serial the slot held. The engine only fills a slot
	// that is empty, so an older serial here is one whose C_HeliGone is
	// still on its way, and the ring above catches it when it lands.
	HeliSlot &s = m_helis[owner].slots[body.slot];
	s.live      = true;
	s.serial    = body.serial;
	return true;
}

bool Session::NoteHeliGone(uint8_t owner, const HeliGoneBody &body) {
	if (owner >= MAX_PLAYERS || !IsPoliceHeliSlot(body.slot) ||
	    !IsKnownHeliGoneReason(body.reason))
		return false;
	if (HeliGoneAlready(owner, body.serial))
		return false;

	OwnerHelis &o = m_helis[owner];
	o.gone[o.goneNext] = body.serial;
	o.goneNext         = static_cast<uint8_t>((o.goneNext + 1) % HELI_GONE_MEMORY);
	if (o.goneCount < HELI_GONE_MEMORY)
		++o.goneCount;

	HeliSlot &s = o.slots[body.slot];
	if (s.live && s.serial == body.serial)
		s = HeliSlot{};
	return true;
}

bool Session::MayCreditHeli(uint8_t owner, uint8_t credit) {
	return credit != owner && FindById(credit) != nullptr;
}

Player *Session::HeliHitRecipient(const HeliHitBody &body, uint8_t byPlayerId) {
	if (!IsSaneHeliHit(body) || body.ownerPlayerId >= MAX_PLAYERS)
		return nullptr;
	if (body.ownerPlayerId == byPlayerId)
		return nullptr;   // the owner's engine already took its own hit
	const HeliSlot &s = m_helis[body.ownerPlayerId].slots[body.slot];
	if (!s.live || s.serial != body.serial)
		return nullptr;
	return FindById(body.ownerPlayerId);
}

bool Session::HeliLive(uint8_t owner, uint8_t slot, uint16_t serial) const {
	if (owner >= MAX_PLAYERS || !IsPoliceHeliSlot(slot))
		return false;
	const HeliSlot &s = m_helis[owner].slots[slot];
	return s.live && s.serial == serial;
}

bool Session::MayRelayHeliShot(uint8_t owner, const HeliShotBody &body) const {
	if (!IsSaneHeliShot(body))
		return false;
	// Only a helicopter the owner is streaming. A round that overtook its
	// helicopter's C_HeliGone, or came before its first state, has no replica
	// anywhere to be drawn from.
	return HeliLive(owner, body.slot, body.serial);
}

std::string SanitizeText(const char *src, size_t capacity) {
	std::string out;
	out.reserve(capacity);
	for (size_t i = 0; i < capacity && src[i] != '\0'; ++i) {
		const unsigned char c = static_cast<unsigned char>(src[i]);
		out.push_back(c < 0x20 ? ' ' : src[i]);
	}
	return out;
}

} // namespace coopiii
