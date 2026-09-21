// Session state: who's connected, their ids, and the shared world clock.
// Transport-agnostic on purpose: Session never touches ENet, it just returns
// what should be sent. Makes it testable without a game or a socket.
#pragma once

#include "coopiii/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coopiii {

struct Player {
	bool        active = false;
	uint32_t    peer   = 0;
	uint8_t     id     = INVALID_PLAYER;
	uint16_t    netId  = INVALID_NETID;
	std::string nick;
	uint16_t    modelId = 0;
	Vec3        pos     = {};
	float       heading = 0.0f;
	uint16_t    vehicleNetId = INVALID_NETID;   // vehicle being driven, if any
};

// A vehicle the session knows about, meaning one a player has actually been
// in at some point.
//
// Server keeps the identity (so it can tell later joiners to spawn it) and
// the last transform it heard (so they spawn it where it is now, not where
// it got claimed). No simulation happens here; the driver is authoritative
// and a car with nobody in it just sits there.
struct Vehicle {
	bool     active  = false;
	uint16_t netId   = INVALID_NETID;
	uint16_t modelId = 0;
	uint8_t  colour1 = 0, colour2 = 0;
	Vec3     pos = {};
	Quat     rot = {0.0f, 0.0f, 0.0f, 1.0f};
	uint8_t  driverPlayerId = INVALID_PLAYER;
};

// Advances one in-game minute per 1000 real ms, matching the game's own rate
// (CClock::Initialise(1000), re3 src/core/Game.cpp:596).
class GameClock {
public:
	static constexpr uint32_t MS_PER_GAME_MINUTE = 1000;

	GameClock(uint8_t hour = 12, uint8_t minute = 0) : m_hour(hour), m_minute(minute) {}

	void Advance(uint32_t elapsedMs);
	uint8_t Hour() const { return m_hour; }
	uint8_t Minute() const { return m_minute; }

private:
	uint8_t  m_hour;
	uint8_t  m_minute;
	uint32_t m_accumMs = 0;
};

class Session {
public:
	explicit Session(uint8_t maxPlayers = MAX_PLAYERS);

	// Returns the assigned player, or nullptr if rejected. `reject` says why.
	Player *AddPlayer(uint32_t peer, const char *nick, uint16_t modelId,
	                  uint16_t protocolVersion, RejectReason &reject);
	// Returns the id freed, or INVALID_PLAYER if the peer wasn't a player.
	uint8_t RemovePeer(uint32_t peer);

	Player *FindByPeer(uint32_t peer);
	Player *FindById(uint8_t id);

	uint16_t AllocNetId() { return m_nextNetId++; }

	const std::vector<Player> &Players() const { return m_players; }
	uint8_t Count() const;

	GameClock &Clock() { return m_clock; }
	uint8_t Weather() const { return m_weather; }
	void SetWeather(uint8_t w) { m_weather = w; }

	// ---- vehicles ---------------------------------------------------------
	//
	// A vehicle joins the session the first time a player gets into it, and
	// stays for the rest of the session. Park it and it's still there when
	// you walk back. Getting out doesn't remove it. See EnterVehicleBody in
	// protocol.h for why the rest of the world's cars aren't synced at all.
	Vehicle       *FindVehicle(uint16_t netId);
	const Vehicle *FindVehicle(uint16_t netId) const;

	// Registers a newly claimed vehicle, returns it, or null if the session
	// already has as many as it'll track.
	Vehicle *AddVehicle(uint16_t modelId, uint8_t colour1, uint8_t colour2,
	                    const Vec3 &pos, const Quat &rot);

	const std::vector<Vehicle> &Vehicles() const { return m_vehicles; }

private:
	std::vector<Player>  m_players;
	std::vector<Vehicle> m_vehicles;
	GameClock            m_clock;
	uint8_t              m_weather   = 0;   // WEATHER_SUNNY
	uint16_t             m_nextNetId = 1;   // 0 is INVALID_NETID
};

// Trims to capacity and guarantees NUL termination. Returns the clean string.
std::string SanitizeText(const char *src, size_t capacity);

} // namespace coopiii
