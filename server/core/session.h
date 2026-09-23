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

	// What this player is carrying, per inventory slot, as last reported.
	//
	// Written down rather than only relayed, for the reason the model index
	// is (OnPlayerModel): a C_PlayerAmmo only ever reaches whoever was
	// connected when it was sent, so without this a late joiner is told
	// nothing about the twelve weapons everybody else's copy of this player
	// has the right numbers for. `known[w]` is false until the owner has
	// said something about slot w.
	//
	// The held weapon is not in here and does not need to be: its count
	// rides every snapshot (PlayerStateBody::ammoTotal), so the joiner has
	// it 40 ms after the backfill.
	uint32_t    ammoTotal[INVENTORY_SLOTS] = {};
	uint16_t    ammoClip[INVENTORY_SLOTS]  = {};
	bool        ammoKnown[INVENTORY_SLOTS] = {};

	// Purely so the refusal in OnVehicleState says itself once per player
	// rather than once per dropped snapshot. Cleared when they get into a
	// car, because at that point the old complaint is about a car they are
	// no longer anywhere near.
	bool        warnedVehicleAuthority = false;

	// Same idea, for the two ways an ambient-ped claim gets refused. The
	// thing on the other end of that stream is a pedestrian generator, so a
	// per-claim complaint would be a per-frame complaint.
	bool        warnedPedCap    = false;
	bool        warnedPedTempId = false;
	// And the same two for traffic, which has the same shape of generator
	// behind it.
	bool        warnedCarCap    = false;
	bool        warnedCarTempId = false;

	// Which garages this player's own machine has away from rest, one bit
	// per garage (docs/protocol.md §1.16).
	//
	// Held by the session for one reason only: a joiner has to be told. The
	// mask is a *level* and it is sent on change, so somebody who connects
	// while another player is standing in their safehouse would otherwise
	// never hear about that door until its owner walked away - at which point
	// they would be told it had closed, having never been told it was open.
	// The server arbitrates nothing here; a garage belongs to the map and
	// there is no owner to check a report against.
	uint32_t    garageMask = 0;

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

	// CVehicle::m_aExtras, -1 for an empty slot. Part of the identity for the
	// same reason the colours are: the engine rolls a car's extra components
	// per machine at spawn, so a joiner told only the model builds a car with
	// different bits on it from everybody else's. Set by the claim rather
	// than by AddVehicle, to keep this out of Session's signature.
	int8_t   extra1 = -1, extra2 = -1;

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

	// What shape it is in: CDamageManager's panel word and six two-bit door
	// levels (docs/cardamage.md). Health said how close the car was to
	// exploding and never said anything about whether it still had a boot, so
	// every joiner was handed a showroom-fresh body on a car that had been
	// through a wall.
	//
	// Accumulated with a componentwise maximum rather than assigned, and that
	// is not defensiveness: every ladder in CDamageManager climbs and none
	// descends, so max is what the engine itself would have produced. It makes
	// the record independent of who reported what and in what order, which is
	// what lets the ownerless kinds in roadmap.md §5.8 reuse the same rules.
	uint32_t damagePanels = 0;
	uint16_t damageDoors  = 0;

	// Blown up. Kept as a row rather than deleted so the netId stays spoken
	// for: a snapshot still in flight for a car that has just exploded must
	// not be able to register a *second* car under the same number.
	//
	// A destroyed car is left out of the backfill entirely. That is a
	// decision and it is reversible in one branch - see BuildBackfill.
	bool     destroyed = false;
};

// A pedestrian one player's engine made, that the whole session now shares.
//
// Unlike a Vehicle, this row is owned: an ambient ped exists because one
// machine's CPopulation made it, that machine runs its AI, and when that
// machine's engine takes it away it is gone everywhere. So `ownerPlayerId`
// is not "who was last in it" - it is who decides, and it never changes.
// docs/population.md §1.1. There is no handoff, deliberately: the design
// says not to promise one on the strength of somebody else's example.
//
// Which means a player leaving takes their peds with them. That is a real
// flicker in the street and it is the honest behaviour - the alternative is
// every observer keeping a ped nobody is simulating.
struct AmbientPed {
	bool     active  = false;
	uint16_t netId   = INVALID_NETID;
	uint8_t  ownerPlayerId = INVALID_PLAYER;
	AmbientPedBody body{};

	// Dead, and the animation his host's engine chose for it. Kept rather
	// than relayed and forgotten - which is what a limb is - for one reason:
	// a corpse lies in the street for the minute or so CPopulation takes to
	// reap it, and anybody joining inside that minute would otherwise be
	// handed a pedestrian standing up in a pool of everyone else's blood.
	//
	// The server does not work this out, it is told. An ambient ped's health
	// lives on the machine hosting it and nowhere else - it is not even on
	// the wire (protocol.h, AmbientPedState) - so the host is the only thing
	// that can say when it ran out. Same split as a Player's `alive`.
	bool     alive       = true;
	uint16_t deathAnimId = ANIM_NONE;
};

// How many ambient peds the session will track at once.
//
// A cap rather than an unbounded vector, because the thing on the other end
// of this is an engine that generates pedestrians forever. GTA III's own ped
// pool is 140 slots and CPopulation keeps 25-odd of them alive per machine;
// this is that, times the player cap, with room to spare. Past it the server
// refuses the claim and says so once - a refused ped stays a purely local
// pedestrian on its creator's machine, which is exactly the behaviour
// CoopIII had before any of this existed.
constexpr size_t MAX_AMBIENT_PEDS = 256;

// A traffic car one player's engine made, that the whole session now shares.
//
// The same row as an AmbientPed, owned the same way and for the same reasons,
// with one member a ped has no use for: `pos`/`rot` are kept current from the
// owner's C_CarStates stream, because a traffic car is going somewhere and a
// joiner who is handed the position it was *born* at gets a car in the middle
// of a junction it drove out of two minutes ago.
//
// Kept on the server rather than only relayed for exactly that: the backfill
// is the only reader. Nothing here arbitrates a car's movement - the owner
// decides where its own cars are, and this is a copy of the last thing they
// said.
struct AmbientCar {
	bool     active  = false;
	uint16_t netId   = INVALID_NETID;
	uint8_t  ownerPlayerId = INVALID_PLAYER;
	AmbientCarBody body{};

	// Its host's engine destroyed it (docs/roadmap.md 5.8, the ambient half).
	// The row stays so the netId remains spoken for and the host's despawn
	// still lands on it; what changes is that the car is left out of the
	// backfill, for the same reason a wrecked session car is - nothing in
	// S_CarSpawn can describe a burnt shell, and a joiner handed the intact
	// car would be the only person in the session who could see it.
	bool     destroyed = false;
};

// Fewer than the peds, and lower than the vehicle cap for a different reason
// than the vehicle cap exists.
//
// GTA III's whole vehicle pool is 110 slots and CCarCtrl keeps a dozen-odd
// traffic cars alive per machine. 128 is that times the player cap with room
// over; past it a claim is refused and the car stays purely local to the
// machine that made it.
constexpr size_t MAX_AMBIENT_CARS = 128;

// Everything a player who joins a session already in progress has to be told
// to end up with the same world as the people who were here first.
//
// Returned as data rather than sent, because the interesting question - what
// goes in it - is worth testing without a socket, and because "which packets"
// is a session decision while "how to send them" is not. Order is significant
// and it is the order of the members: a seat needs both a player and a car to
// already exist on the far side, and all three ride the reliable ordered
// channel so the order they leave in is the order they arrive in.
// A car nobody owns that somebody's engine has already destroyed.
//
// docs/roadmap.md 5.8. There is no Vehicle row for one of these and there
// never will be: a parked car was placed by the map on every machine, so the
// session has nothing to say about where it is or what it looks like. The one
// fact it holds is that it is a wreck.
//
// ---- why this expires, which is the only interesting decision in it ----
//
// A generator makes a new car once the old one has left the vehicle pool, and
// CCarCtrl::PossiblyRemoveVehicle takes a wreck out 60 seconds after its
// m_nTimeOfDeath (client addresses.h, "the reaping site"). So a record older
// than that describes a car that is gone from every machine, and replaying it
// to a joiner would wreck whatever fresh car the generator has since put in
// the same parking space - turning a late joiner's one wrong car into a
// permanent stream of them.
//
// The residual, said out loud because it is the price: the engine's removal
// also wants the wreck off-screen and far from the player, so a wreck someone
// is standing next to outlives the record. A joiner arriving in that window
// gets the old behaviour for that one car - an intact car where everybody
// else has a shell - and it corrects itself the moment anything touches it.
// Trading a rare wrong car for a guaranteed stream of wrong cars.
struct WreckedUnownedCar {
	UnownedVehicleKey key      = {};
	uint8_t           byPlayer = INVALID_PLAYER;   // for the log, nothing else
	uint32_t          atMs     = 0;
};

// The engine's own number, from CCarCtrl::PossiblyRemoveVehicle's wreck
// branch: `add eax,0EA60h` on m_nTimeOfDeath. Not a tuning constant.
constexpr uint32_t WRECK_BACKFILL_MS = 60000;

struct Backfill {
	std::vector<S_PlayerJoin>   players;
	std::vector<S_VehicleSpawn> vehicles;
	std::vector<S_EnterVehicle> seats;
	// Last, and carrying tempId 0 throughout: these are peds the joiner
	// certainly did not create, so nothing in them may look like an answer
	// to a claim of its own.
	std::vector<S_PedSpawn>     peds;
	// And which of those pedestrians are already lying in the road. After
	// the spawns and never merged into them: S_PedSpawn has no way to say
	// "corpse", and the joiner's own reconciliation loop is what turns one
	// into the other once the model has streamed - the death is recorded on
	// the row and carried out on the first frame there is a ped to carry it
	// out on, exactly as a live death is.
	std::vector<S_PedDeath>     pedDeaths;
	// And the traffic, beside the pedestrians and for the same reasons,
	// tempId 0 throughout. Each one carries the position the session last
	// heard rather than the one the car was created at - see AmbientCar.
	std::vector<S_CarSpawn>     cars;
	// Which pickups are currently gone. Without it a joiner walks into a
	// world where every hidden package the group has already collected is
	// still spinning, and collects the first one they touch - which would
	// then be denied, over and over, with the package visibly there.
	std::vector<S_PickupTaken>  pickups;
	// And which unowned cars are already wrecks. Same idea as the pickups
	// immediately above: a joiner who is not told walks into a street full of
	// cars everyone else watched burn.
	std::vector<S_UnownedBlowUp> unownedWrecks;
	// And what shape each of those cars is in. Separate from the spawn packet
	// rather than two more fields on it, because the client applies damage
	// through a different path from the one that builds a car - it has to
	// write the status *and* call the engine's own applier for each component
	// - and because keeping S_VehicleSpawn's layout alone keeps this change to
	// two new opcodes. docs/cardamage.md §3.4.
	std::vector<S_VehicleDamage> vehicleDamage;
	// Which doors every other player currently has away from rest. One per
	// player with a non-zero mask, skipped entirely for the common case where
	// nobody is standing in a garage.
	//
	// This is the whole reason the session remembers a mask at all. The mask
	// is a level and it is sent on change, so without this a joiner would
	// hear nothing until the door closed - and would then be told a door they
	// never saw open had shut.
	std::vector<S_GarageState>  garages;
	// What everybody already in the session is carrying, one packet per slot
	// anybody has said anything about. Empty unless the session has ammo
	// sync on, and it never covers the weapon a player is holding - that one
	// is in their next snapshot, 40 ms away.
	std::vector<S_PlayerAmmo>    ammo;
};

// A pickup somebody has, and until when.
//
// docs/pickups.md 4 and 5: the server owns *availability* and each machine's
// own engine owns *appearance*. That split is the reason this record exists
// rather than a respawn deadline living on each client - CTimer is local, it
// pauses, and two machines' copies of a pickup genuinely do come back at
// different moments. None of that matters as long as nobody can be awarded
// one twice.
//
// A record is a lock, not a tombstone. The script destroys and re-creates
// pickups at the same coordinate - every one of the 20 rampages returns to
// its original spot after two failures - so a client whose engine has a live
// object at a key it previously removed says so with C_PickupRelease and the
// record goes.
struct TakenPickup {
	// RESERVED means somebody is standing near it and has been told they may
	// have it; nobody has picked anything up and no other machine has been
	// told anything. TAKEN means they did.
	//
	// The two are separate because the claim goes out on *approach*, at 4 m,
	// which is what removes the race - and claiming on approach means
	// claiming things you turn out not to want. A player at full health walks
	// past a health pickup, an armour they decided against, or simply down a
	// street. If a reservation removed the pickup from everyone else's world
	// they would be deleting pickups by walking.
	enum class State : uint8_t { RESERVED, TAKEN };

	PickupIdent ident;
	State       state      = State::RESERVED;
	uint8_t     byPlayerId = INVALID_PLAYER;
	uint32_t    sinceMs    = 0;   // server monotonic; reserved or taken at
	uint32_t    respawnMs  = 0;   // 0 = never comes back
};

// How long a reservation stands before the server takes it back.
//
// A holder releases explicitly when they walk out of the claim radius, so
// this only fires when the holder never says anything again - a client that
// crashed, or a connection that went away between the grant and the release.
// Generous, because the cost of being wrong is one pickup nobody can take for
// a few seconds, and the cost of being too eager is two players being granted
// the same one.
constexpr uint32_t PICKUP_RESERVATION_MS = 15000;

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

	// Whether the session reports ammunition honestly (protocol.h,
	// SESSION_AMMO_SYNC). Server-configurable, off by default, same shape as
	// friendly fire above: the server refuses to relay a C_PlayerAmmo with
	// it off, and tells every client in S_Welcome so they know whether the
	// two snapshot fields mean anything.
	//
	// It is not a shared inventory. Two players carrying different weapons
	// is the normal case and nothing here changes it - all this decides is
	// whether everybody's copy of a player has that player's real numbers.
	bool AmmoSync() const { return m_ammoSync; }
	void SetAmmoSync(bool on) { m_ammoSync = on; }

	// Record what a player says is in one of their weapon slots. Returns
	// false when the slot is out of range or nothing changed, which is what
	// stops the server relaying a restatement.
	bool NoteAmmo(Player &p, const AmmoSlotBody &slot) {
		if (slot.weapon >= INVENTORY_SLOTS)
			return false;

		// "I no longer have this weapon" is worth relaying to everyone who
		// is here, and worth forgetting rather than writing down: a joiner
		// who is told nothing about a slot already believes it is empty, so
		// backfilling an absence would be sending a packet to say nothing.
		if (!(slot.flags & AMMO_SLOT_OWNED)) {
			if (!p.ammoKnown[slot.weapon])
				return false;
			p.ammoKnown[slot.weapon] = false;
			p.ammoTotal[slot.weapon] = 0;
			p.ammoClip[slot.weapon]  = 0;
			return true;
		}

		if (p.ammoKnown[slot.weapon] && p.ammoTotal[slot.weapon] == slot.total &&
		    p.ammoClip[slot.weapon] == slot.clip)
			return false;
		p.ammoKnown[slot.weapon] = true;
		p.ammoTotal[slot.weapon] = slot.total;
		p.ammoClip[slot.weapon]  = slot.clip;
		return true;
	}

	// docs/roadmap.md §5.1 and docs/wanted.md §4.8: which of the three wanted
	// rules this session runs. The value is a protocol.h WantedRule, not the
	// server's own WantedLevelRule enum - the two have the same three values
	// on purpose, and keeping the wire type here is what stops a cast being
	// needed at the one place it actually goes out.
	//
	// Unlike friendly fire there is no server-side half of this at all. The
	// wanted level lives inside a client's engine and never passes through
	// here as anything but a number in a snapshot, so there is nothing to
	// refuse. Saying which rule is in force is the server's entire part.
	uint8_t WantedRule() const { return m_wantedRule; }
	void SetWantedRule(uint8_t rule) {
		m_wantedRule = rule > WANTED_RULE_OFF ? uint8_t(WANTED_RULE_PERPLAYER) : rule;
	}

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

	// ---- ambient peds ------------------------------------------------------
	//
	// docs/population.md §1.2. The server's whole job here is naming: a
	// client announces a ped it has already created under a temporary id of
	// its own, and this hands back the one name everybody will use.

	AmbientPed       *FindPed(uint16_t netId);
	const AmbientPed *FindPed(uint16_t netId) const;

	// Registers a ped `ownerPlayerId` created. Null when the session is at
	// MAX_AMBIENT_PEDS, which the caller must report rather than ignore.
	AmbientPed *AddPed(uint8_t ownerPlayerId, const AmbientPedBody &body);

	// Forgets a ped. `byPlayerId` must be its owner; anyone else is refused
	// and gets false, because an observer losing its replica says nothing
	// about the entity. INVALID_PLAYER means the session itself is removing
	// it (a player left), and that is always allowed.
	bool RemovePed(uint16_t netId, uint8_t byPlayerId);

	// Every netId `playerId` owns. Used when they disconnect: their peds go
	// with them, since nothing is left to simulate any of them.
	std::vector<uint16_t> PedsOwnedBy(uint8_t playerId) const;

	// Writes one streamed pose into the session's copy, so a joiner is
	// handed a pedestrian where he is now rather than where he was born two
	// minutes and a street corner ago. Exactly NoteCarState's job, and the
	// same host-authoritative refusal: a snapshot about somebody else's ped
	// is a statement about the sender, not about the ped.
	//
	// The seat is deliberately *not* recorded. A backfilled ped arrives with
	// a position and nothing else; his driver link comes on his owner's very
	// next batch, 100 ms later, through the same reconciliation loop that
	// heals every other race. Storing it would mean the session keeping a
	// pairing whose other half it may already have removed.
	bool NotePedState(const AmbientPedState &state, uint8_t byPlayerId);

	// The host says one of its pedestrians died. Records it so the backfill
	// can hand a joiner a corpse, and refuses a claim about somebody else's
	// ped - the same host-authoritative rule RemovePed and NotePedState
	// enforce, and the reason it is enforced here rather than in the relay.
	//
	// False for a ped that is not there or not the sender's, and false for
	// one that is already dead: a second death for one life is either a
	// duplicate or a host that has lost track, and relaying it would run
	// CPed::SetDie a second time over a state that is already the death's.
	bool NotePedDeath(const PedDeathBody &death, uint8_t byPlayerId);

	const std::vector<AmbientPed> &Peds() const { return m_peds; }

	// ---- ambient traffic ---------------------------------------------------
	//
	// docs/population.md §3 step 4. Identical in shape to the ped block above
	// and deliberately not merged with it: the two have different caps, and a
	// car additionally has a position the session keeps current.

	AmbientCar       *FindCar(uint16_t netId);
	const AmbientCar *FindCar(uint16_t netId) const;

	// Registers a car `ownerPlayerId` created. Null at MAX_AMBIENT_CARS.
	AmbientCar *AddCar(uint8_t ownerPlayerId, const AmbientCarBody &body);

	// Forgets a car. Same ownership rule as RemovePed, same reason.
	bool RemoveCar(uint16_t netId, uint8_t byPlayerId);

	std::vector<uint16_t> CarsOwnedBy(uint8_t playerId) const;

	// Writes one streamed transform into the session's copy, so the backfill
	// hands a latecomer the car where it is now rather than where it was
	// born. Refuses a car `byPlayerId` does not own and returns false, which
	// is the same host-authoritative rule RemoveCar enforces: a snapshot
	// about somebody else's car is a statement about the sender, not the car.
	bool NoteCarState(const AmbientCarState &state, uint8_t byPlayerId);

	const std::vector<AmbientCar> &Cars() const { return m_cars; }

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

	// Fold a damage report into a car's record and say whether it added
	// anything. False means the session already knew - a duplicate, or a
	// report that has been overtaken by a worse one - and nothing is relayed,
	// which is what keeps a monotone state from becoming a stream.
	//
	// `out` is filled with the merged record rather than with what arrived, so
	// everybody downstream is told what the session now believes rather than
	// what one machine happened to see.
	bool NoteVehicleDamage(const VehicleDamageBody &in, VehicleDamageBody &out);

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

	// ---- pickups -----------------------------------------------------------
	//
	// The one thing the server decides about a pickup, and the only thing it
	// can: which of two players standing on the same shotgun gets it. Both of
	// them are correctly answering "am I on it" about themselves - that is a
	// place, and a place is a question you answer about yourself
	// (docs/roadmap.md 5.7) - but the resource is exclusive, so somebody has
	// to arbitrate and only the server sees both claims.
	//
	// There is no spawn half. Every machine runs main.scm and creates all 448
	// script pickups itself from literal coordinates, so the worlds already
	// agree. docs/pickups.md 1.

	// First claim wins. GRANTED means nobody holds it and the window on any
	// previous holder has passed; the caller replies S_PickupGrant to the
	// claimant and says nothing to anybody else, because at that point
	// nothing has been picked up.
	enum class PickupVerdict : uint8_t { GRANTED, DENIED };
	PickupVerdict ClaimPickup(uint8_t playerId, const PickupIdent &ident,
	                          uint32_t nowMs);

	// The holder's engine actually took it. Turns their reservation into a
	// removal and starts the respawn window; the caller then broadcasts
	// S_PickupTaken to everybody *except* them, since their own engine has
	// already removed their copy.
	//
	// False when they do not hold it - a duplicate report, or a collection
	// claimed by somebody who was never granted it. The session is not
	// interested in either, and a false is the caller's cue to broadcast
	// nothing.
	bool NotePickupCollected(uint8_t playerId, const PickupIdent &ident,
	                         uint32_t nowMs);

	// Every reservation this player holds, dropped. Their *collections* stay:
	// they took those, and the window runs on whether or not they are still
	// connected. Called when they leave.
	void ReleaseReservationsOf(uint8_t playerId);

	// Let go of a record: a grant the client could not consume, or a key the
	// script has re-created. Safe to call for a key nobody holds.
	void ReleasePickup(const PickupIdent &ident);

	// Drop records whose respawn window has passed, and reservations nobody
	// has said anything about for PICKUP_RESERVATION_MS. Called on the
	// server's own tick; a *taken* record with respawnMs == 0 never expires
	// and is only removed by a release.
	void ExpirePickups(uint32_t nowMs);

	// A player leaving does not give their pickups back. They collected them;
	// the window runs on regardless of whether they are still connected.
	// Here so that intent is written down rather than implied by the absence
	// of a call in RemovePeer.

	const std::vector<TakenPickup> &TakenPickups() const { return m_pickups; }

	// Same identity rule as the client: same model, within 0.25 m. Tolerance
	// rather than an exact float compare, because a ped drop's z comes out of
	// CWorld::FindGroundZFor3DCoord on one machine and this must not depend
	// on that landing on the same bit in two processes.
	static bool SameIdent(const PickupIdent &a, const PickupIdent &b);

	// ---- cars nobody owns --------------------------------------------------
	//
	// docs/roadmap.md 5.8. A parked car is not a session entity and never
	// becomes one: no netId, no row, no spawn packet. The only thing the
	// session needs to remember about one is that it is finished - so this is
	// a set of keys and a timestamp, not a Vehicle.
	//
	// First report wins and the rest are dropped, which is the pickup rule in
	// a place where it costs nothing: nobody is waiting on the answer and a
	// duplicate is not a contest, it is two machines correctly noticing the
	// same explosion. Returns false for a key already recorded, and that
	// false is the caller's cue to relay nothing.
	bool NoteUnownedBlowUp(const UnownedVehicleKey &key, uint8_t byPlayerId,
	                       uint32_t nowMs);

	// Drop records old enough that every machine's engine has had time to take
	// the wreck out of the pool and let the generator make a fresh car. See
	// WRECK_BACKFILL_MS.
	void ExpireUnownedWrecks(uint32_t nowMs);

	const std::vector<WreckedUnownedCar> &UnownedWrecks() const {
		return m_unownedWrecks;
	}

	static bool SameUnownedKey(const UnownedVehicleKey &a,
	                           const UnownedVehicleKey &b) {
		return a.kind == b.kind && a.id == b.id;
	}

private:
	std::vector<Player>     m_players;
	std::vector<Vehicle>    m_vehicles;
	std::vector<AmbientPed> m_peds;
	std::vector<AmbientCar> m_cars;
	GameClock            m_clock;
	uint8_t              m_weather    = 0;   // WEATHER_SUNNY
	uint8_t              m_weatherOld = 0;
	uint8_t              m_hostId     = INVALID_PLAYER;
	uint16_t             m_nextNetId  = 1;   // 0 is INVALID_NETID
	bool                 m_friendlyFire = false;   // docs/roadmap.md §5.2
	bool                 m_ammoSync     = false;   // docs/protocol.md 1.9.6
	uint8_t              m_wantedRule   = WANTED_RULE_PERPLAYER;  // §5.1
	std::vector<TakenPickup> m_pickups;
	std::vector<WreckedUnownedCar> m_unownedWrecks;

	// Found by ident, or null. Non-const so ClaimPickup can overwrite a
	// record whose window has passed instead of growing the vector forever.
	TakenPickup *FindPickup(const PickupIdent &ident);

	// Called after the roster changes. Keeps the host on the lowest active
	// slot, and clears it when the session empties out.
	void PickHost();
};

// Trims to capacity and guarantees NUL termination. Returns the clean string.
std::string SanitizeText(const char *src, size_t capacity);

} // namespace coopiii
