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
	// The car this player is in, and where in it, or INVALID_NETID for on
	// foot. Not "the car they are driving": a passenger is in a car too.
	//
	// `seat` used to be dropped on the floor, and dropping it is the bug this
	// area is about in its purest form. A passenger's seat has exactly one
	// carrier, the S_EnterVehicle that announced it, and an event only ever
	// reaches whoever was connected when it was sent. So everybody already in
	// the session saw the passenger get in, and the next player through the
	// door was told about a car with an empty passenger seat and a player
	// jogging along beside it. Nothing was wrong with the client - it has
	// carried `seat` end to end since protocol 3 - the session simply never
	// wrote the number down.
	uint16_t    vehicleNetId = INVALID_NETID;
	uint8_t     seat         = 0;               // 0 is the driver

	// Purely so the refusal in OnVehicleState says itself once per player
	// rather than once per dropped snapshot. Cleared when they get into a
	// car, because at that point the old complaint is about a car they are
	// no longer anywhere near.
	bool        warnedVehicleAuthority = false;

	// Whether `pos`/`heading` mean anything yet.
	//
	// A Player is born at the origin because a struct has to start somewhere,
	// and the origin in Liberty City is the water off Portland. Announcing
	// that as a position would have every joiner create a ped there and watch
	// it drown, which is a bug this project has already had once and paid for
	// (AGENTS.md, "the ped was born in water"). So the session says whether
	// it knows, and the packet carries a bit for it.
	bool        havePos = false;

	// Whether this player has told us they're dead.
	//
	// The server doesn't work this out, it's told: a player's health lives on
	// their own machine and nowhere else (docs/protocol.md §1.10). What it's
	// for is refusing to relay a hit onto somebody who is already on the
	// floor, so a burst that arrives a moment after a death doesn't get
	// applied to a corpse and count as a second kill - and, since version 9,
	// telling a joiner that the body in the road is a body.
	bool        alive = true;
	// The animation their engine chose for that death, straight off their
	// C_Death, so a backfilled corpse lies the way it fell.
	uint16_t    deathAnimId = ANIM_NONE;

	// Condition, kept off the snapshot stream purely so a joiner can be told
	// it. Nothing on the server reads these; they exist to be replayed.
	//
	// Keeping a copy is what makes a join packet describe a player who is
	// *not currently sending snapshots* - the frontend, a loading screen, a
	// cutscene. For everyone else it saves the joiner one snapshot interval
	// of wrongness, which matters more than it sounds: the ped is created
	// from this, so 40 ms of "wrong" is a ped born with the wrong health.
	float       health = 100.0f;
	float       armour = 0.0f;
	uint8_t     weapon = 0;      // eWeaponType
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

	// ---- condition ---------------------------------------------------------
	//
	// The identity above is what the car *is*; this is what has happened to
	// it. The session used to keep only the first, so the backfill rebuilt
	// every car showroom-fresh no matter what it had been through - which is
	// the bug this whole area came from.
	float    health = 1000.0f;   // CVehicle::m_fHealth, 1000 = full
	uint8_t  flags  = 0;         // VehicleFlags, as the driver last reported

	// Blown up. Kept as a row rather than deleted so the netId stays spoken
	// for: a snapshot still in flight for a car that has just exploded must
	// not be able to register a *second* car under the same number.
	//
	// A destroyed car is left out of the backfill entirely. That is a
	// decision and it is reversible in one branch - see BuildBackfill.
	bool     destroyed = false;
};

// Everything a player who joins a session already in progress has to be told
// to end up with the same world as the people who were here first.
//
// Returned as data rather than sent, because the interesting question - what
// goes in it - is worth testing without a socket, and because "which packets"
// is a session decision while "how to send them" is not. Order is significant
// and it is the order of the members: a seat needs both a player and a car to
// already exist on the far side, and all three ride the reliable ordered
// channel so the order they leave in is the order they arrive in.
struct Backfill {
	std::vector<S_PlayerJoin>   players;
	std::vector<S_VehicleSpawn> vehicles;
	std::vector<S_EnterVehicle> seats;
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

	// ---- keeping the session's copy current --------------------------------
	//
	// Three notes, one per stream that changes something a joiner would
	// otherwise never hear about. They live here rather than in main.cpp so
	// the rule ("what does the session remember from this packet") is the
	// thing under test, not the relay around it.

	// A player's own report of themselves. Records the condition fields the
	// backfill replays, and the position, and marks the position real.
	void NotePlayerState(Player &p, const PlayerStateBody &body);

	// A driver's report of the car they're in. Records where it is and what
	// shape it's in, and notices a car that has been destroyed.
	void NoteVehicleState(const VehicleStateBody &body);

	// May `playerId` tell the session what condition `netId` is in?
	//
	// Only its recorded driver, and that is stricter than it used to be on
	// purpose. The old gate was "drop this if we think they are in some
	// *other* car", which let the snapshot through whenever the session
	// happened to think the sender was on foot - a real window, since a
	// client can get one more snapshot away after an exit.
	//
	// Waving a stray position through was survivable. Waving the condition
	// fields through is not: as of version 9 the same packet carries
	// VEH_WRECKED, so the permissive branch was a way for any player in the
	// session to delete any car from every future backfill. A snapshot is
	// believed because of who sent it, not because nothing contradicts it.
	bool MayReportVehicle(uint8_t playerId, uint16_t netId) const;

	// A player got into a car, in a seat, or got out of one. The only place
	// the session writes down who is sitting where, so the backfill and the
	// live fan-out cannot disagree about it. NoteExitVehicle is safe to call
	// for a car they were never in.
	void NoteEnterVehicle(Player &p, Vehicle &v, uint8_t seat);
	void NoteExitVehicle(Player &p, uint16_t netId);

	// "That car is finished." The one way a vehicle becomes destroyed, so
	// there is one place to look and one place for the vehicle seam's own
	// destruction event to land when it arrives (see the report and
	// docs/roadmap.md §5.8). Takes the driver out of it on the way.
	void DestroyVehicle(uint16_t netId);

	// A player died, or came back. Both clear the seat, because a dead player
	// is taken out of their car on every machine that was watching and the
	// session has to agree or the next joiner gets told to put them back in.
	void NotePlayerDied(Player &p, uint16_t deathAnimId);
	void NotePlayerRespawned(Player &p, const Vec3 &pos, float heading);

	// The announcement for one player: who they are, and what condition
	// they're in. Used both for the live "someone joined" fan-out and for
	// every entry in a backfill, so the two can never disagree about the
	// shape of a player.
	S_PlayerJoin MakeJoin(const Player &p, uint32_t sendTimeMs) const;

	// Everything `joinerId` has to be told about the session that was already
	// running. Excludes the joiner themselves.
	Backfill BuildBackfill(uint8_t joinerId, uint32_t sendTimeMs) const;

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
