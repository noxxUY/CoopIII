#include "session.h"

#include <algorithm>

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
	*p               = Player{};
	p->id            = id;
	PickHost();
	return id;
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
