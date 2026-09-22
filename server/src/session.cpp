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

void Session::NoteEnterVehicle(Player &p, Vehicle &v, uint8_t seat) {
	// Out of whatever they were in first, so nobody is ever recorded in two
	// cars at once. Stepping straight from one car into another is a single
	// C_EnterVehicle with no exit in front of it, and a client that sends the
	// pair the other way round would otherwise leave the old car believing it
	// still has a driver for the rest of the session.
	if (p.vehicleNetId != INVALID_NETID && p.vehicleNetId != v.netId)
		NoteExitVehicle(p, p.vehicleNetId);

	p.vehicleNetId = v.netId;
	p.seat         = seat;

	// Only the driver's seat carries ownership. A passenger is recorded so a
	// joiner can be told where they are sitting, and gets no say over the
	// car's position or condition - see MayReportVehicle.
	if (seat == 0)
		v.driverPlayerId = p.id;
}

void Session::NoteExitVehicle(Player &p, uint16_t netId) {
	// The car stays in the session either way. Somebody parked it; it did not
	// stop existing.
	if (Vehicle *v = FindVehicle(netId))
		if (v->driverPlayerId == p.id)
			v->driverPlayerId = INVALID_PLAYER;

	if (p.vehicleNetId == netId || netId == INVALID_NETID) {
		p.vehicleNetId = INVALID_NETID;
		p.seat         = 0;
	}
}

bool Session::MayReportVehicle(uint8_t playerId, uint16_t netId) const {
	const Vehicle *v = FindVehicle(netId);
	return v && v->active && v->driverPlayerId == playerId;
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

	return out;
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
	// and an hours-long session would otherwise hand a latecomer a thousand
	// cars to spawn. Way more than 8 players could ever sit in.
	constexpr size_t MAX_VEHICLES = 64;
	if (m_vehicles.size() >= MAX_VEHICLES) {
		bool reused = false;
		for (Vehicle &v : m_vehicles)
			if (!v.active) {
				v       = Vehicle{};
				reused  = true;
				v.active = true;
				v.netId  = AllocNetId();
				v.modelId = modelId;
				v.colour1 = colour1;
				v.colour2 = colour2;
				v.pos     = pos;
				v.rot     = rot;
				return &v;
			}
		if (!reused)
			return nullptr;
	}

	Vehicle v;
	v.active  = true;
	v.netId   = AllocNetId();
	v.modelId = modelId;
	v.colour1 = colour1;
	v.colour2 = colour2;
	v.pos     = pos;
	v.rot     = rot;
	m_vehicles.push_back(v);
	return &m_vehicles.back();
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
