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

	// Whether this player has told us they're dead.
	//
	// The server doesn't work this out, it's told: a player's health lives on
	// their own machine and nowhere else (docs/protocol.md §1.10). What it's
	// for is refusing to relay a hit onto somebody who is already on the
	// floor, so a burst that arrives a moment after a death doesn't get
	// applied to a corpse and count as a second kill.
	bool        alive = true;
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

// The session's idea of the time of day.
//
// It is not the authority. The host player's own CClock is (see HostId
// below), and Set() is how the host's report lands here. What this does is
// carry the time between those reports and give a joiner something sane
// before the first one arrives, so it keeps advancing at the game's own rate
// of one in-game minute per 1000 real ms (CClock::Initialise(1000), which
// the retail exe does with a literal `push 3E8h` at 0x0048C289).
class GameClock {
public:
	static constexpr uint32_t MS_PER_GAME_MINUTE = 1000;

	GameClock(uint8_t hour = 12, uint8_t minute = 0) : m_hour(hour), m_minute(minute) {}

	void Advance(uint32_t elapsedMs);
	// Out-of-range values are dropped rather than clamped: a client sending
	// hour 200 is a client we can't believe about the minute either.
	bool Set(uint8_t hour, uint8_t minute);
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
	// Players are addressed by netId on the wire wherever the thing being
	// addressed is an entity rather than a slot - C_Damage names its victim
	// that way, and so does the killer in a death.
	Player *FindByNetId(uint16_t netId);

	uint16_t AllocNetId() { return m_nextNetId++; }

	// docs/roadmap.md §5.2: server-configurable, off by default. With it off
	// the server simply doesn't relay a C_Damage between players, so no
	// client is ever asked to hurt itself on another's behalf. Clients are
	// told which way it's set in S_Welcome, because one kind of damage never
	// reaches the server at all: an explosion replayed locally.
	bool FriendlyFire() const { return m_friendlyFire; }
	void SetFriendlyFire(bool on) { m_friendlyFire = on; }

	const std::vector<Player> &Players() const { return m_players; }
	uint8_t Count() const;

	GameClock &Clock() { return m_clock; }
	uint8_t Weather() const { return m_weather; }
	uint8_t WeatherOld() const { return m_weatherOld; }
	// The pair CWeather blends between, not one type. See WorldStateBody.
	bool SetWeather(uint8_t weather, uint8_t weatherOld);

	// ---- host -------------------------------------------------------------
	//
	// One connected player is the host, and the session's time of day and sky
	// are whatever that player's game says they are. The server runs no game,
	// so a clock it kept on its own would be nobody's; docs/campaign.md
	// already gives the host this job for the mission script, and the script
	// is one of the things that moves the clock.
	//
	// It's the first player in, it stays theirs for as long as they're
	// connected, and on their way out it passes to the lowest-numbered
	// player still here. Nothing votes on it: one server, one answer.
	uint8_t HostId() const { return m_hostId; }

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
	uint8_t              m_weather    = 0;   // WEATHER_SUNNY
	uint8_t              m_weatherOld = 0;
	uint8_t              m_hostId     = INVALID_PLAYER;
	uint16_t             m_nextNetId  = 1;   // 0 is INVALID_NETID
	bool                 m_friendlyFire = false;   // docs/roadmap.md §5.2

	// Called after the roster changes. Keeps the host on the lowest active
	// slot, and clears it when the session empties out.
	void PickHost();
};

// Trims to capacity and guarantees NUL termination. Returns the clean string.
std::string SanitizeText(const char *src, size_t capacity);

} // namespace coopiii
