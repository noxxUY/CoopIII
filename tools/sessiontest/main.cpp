// Unit tests for server/src/session.cpp, with no socket and no game.
//
//   xmake build sessiontest && xmake run sessiontest
//
// Session had no coverage of its own while it was only a roster and a
// counter: nettest drives two real clients through it and that was enough.
// It now decides which player the whole session takes its time of day from,
// and that decision is the kind that looks right and is wrong in a case
// nobody reaches by hand - a slot getting reused, a clock a minute short of
// a rollover.

#include "session.h"

#include <cstdio>
#include <string>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

Player *Join(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

// One snapshot, as a player's own machine would send it.
PlayerStateBody State(float x, float health, float armour = 0.0f, uint8_t weapon = 0) {
	PlayerStateBody b{};
	b.pos     = {x, 20.0f, 3.0f};
	b.heading = 1.5f;
	b.health  = health;
	b.armour  = armour;
	b.weapon  = weapon;
	return b;
}

VehicleStateBody VehState(uint16_t netId, float x, float health, uint8_t flags = 0) {
	VehicleStateBody b{};
	b.netId  = netId;
	b.pos    = {x, 40.0f, 5.0f};
	b.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	b.health = health;
	b.flags  = flags;
	return b;
}

Vehicle *Claim(Session &s, Player &driver, uint16_t modelId) {
	Vehicle *v = s.AddVehicle(modelId, 3, 4, Vec3{10.0f, 10.0f, 1.0f},
	                          Quat{0.0f, 0.0f, 0.0f, 1.0f});
	if (v)
		s.NoteEnterVehicle(driver, *v, /*seat=*/0);
	return v;
}

const S_PlayerJoin *FindJoin(const Backfill &b, uint8_t playerId) {
	for (const S_PlayerJoin &j : b.players)
		if (j.playerId == playerId)
			return &j;
	return nullptr;
}

const S_EnterVehicle *FindSeat(const Backfill &b, uint8_t playerId) {
	for (const S_EnterVehicle &e : b.seats)
		if (e.playerId == playerId)
			return &e;
	return nullptr;
}

void TestHostIsTheFirstPlayerIn() {
	std::printf("\npicking a host\n");
	Session s;
	Check(s.HostId() == INVALID_PLAYER, "an empty session has no host");

	const Player *alice = Join(s, 1, "alice");
	Check(alice != nullptr && s.HostId() == alice->id, "the first player in gets it");

	const Player *bob = Join(s, 2, "bob");
	Check(bob != nullptr && s.HostId() == alice->id, "and keeps it when someone joins");
}

void TestAJoinerNeverTakesTheHostFromSomeoneStillHere() {
	std::printf("\na reused slot\n");
	Session s;
	Join(s, 1, "alice");   // slot 0
	Join(s, 2, "bob");     // slot 1
	Check(s.HostId() == 0, "alice has it");

	s.RemovePeer(1);
	Check(s.HostId() == 1, "alice leaves, bob takes it");

	// Slot 0 is free again, so the next player in fills it. Handing them the
	// session's clock because their slot number is lower would drag everyone
	// else to whatever time a machine that just connected is showing.
	const Player *carol = Join(s, 3, "carol");
	Check(carol != nullptr && carol->id == 0, "carol lands in the slot alice left");
	Check(s.HostId() == 1, "and bob still has it");
}

void TestTheLastPlayerOutTakesItWithThem() {
	std::printf("\nthe session emptying\n");
	Session s;
	Join(s, 1, "alice");
	Join(s, 2, "bob");

	s.RemovePeer(2);
	Check(s.HostId() == 0, "a non-host leaving changes nothing");

	s.RemovePeer(1);
	Check(s.HostId() == INVALID_PLAYER, "and the last one out leaves nobody holding it");
}

void TestTheClockAcceptsOnlyRealTimes() {
	std::printf("\nsetting the session clock\n");
	GameClock c;
	Check(c.Hour() == 12 && c.Minute() == 0, "it starts at noon, the way CClock does");

	Check(c.Set(3, 30) && c.Hour() == 3 && c.Minute() == 30, "a real time is taken");
	Check(!c.Set(24, 0) && c.Hour() == 3, "hour 24 is refused and changes nothing");
	Check(!c.Set(12, 60) && c.Minute() == 30, "and so is minute 60");
	Check(c.Set(23, 59), "the last minute of the day is a real time");

	// Set clears the part-minute. Without that, a report landing just short
	// of a rollover would tick the minute straight back off again.
	c.Set(12, 0);
	c.Advance(GameClock::MS_PER_GAME_MINUTE - 1);
	Check(c.Minute() == 0, "a fresh minute starts from the report, not from before it");
	c.Advance(2);
	Check(c.Minute() == 1, "and then runs on at the game's own rate");
}

void TestTheClockRollsOver() {
	std::printf("\nmidnight\n");
	GameClock c;
	c.Set(23, 59);
	c.Advance(GameClock::MS_PER_GAME_MINUTE);
	Check(c.Hour() == 0 && c.Minute() == 0, "23:59 plus a minute is midnight");

	c.Set(12, 0);
	c.Advance(GameClock::MS_PER_GAME_MINUTE * 60 * 24);
	Check(c.Hour() == 12 && c.Minute() == 0, "a whole day comes back to where it started");
}

void TestWeatherIsAPairAndIsChecked() {
	std::printf("\nthe session's sky\n");
	Session s;
	Check(s.Weather() == 0 && s.WeatherOld() == 0, "sunny either side to begin with");

	Check(s.SetWeather(2, 1), "rainy out of cloudy is a real pair");
	Check(s.Weather() == 2 && s.WeatherOld() == 1, "and both ends are kept");

	// eWeatherType is 0..3 and CWeather indexes arrays with it without a
	// bounds check, so this would be a crash on every client rather than a
	// wrong sky on one.
	Check(!s.SetWeather(4, 0), "there is no weather type 4");
	Check(!s.SetWeather(0, 200), "and none at 200 either");
	Check(s.Weather() == 2 && s.WeatherOld() == 1, "a refused pair changes nothing");
}

// ---- the backfill ----------------------------------------------------------
//
// One question, asked of everything the session holds: what does a player who
// was here from the start know, and what does a player who joined a minute
// ago know? Every test below is a place where the two used to differ.
//
// The shape of every one of these bugs is the same. The backfill rebuilt an
// object from its *spawn identity* and left its *current condition* to the
// live stream - which works for anything that is still streaming and fails
// completely for anything whose only carrier was an event that has already
// been and gone.

void TestABackfilledPlayerCarriesTheirCondition() {
	std::printf("\nwhat a joiner is told about a player\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	s.NotePlayerState(*alice, State(100.0f, 37.0f, 50.0f, /*weapon=*/6));

	const Backfill back = s.BuildBackfill(bob->id, 1234);
	const S_PlayerJoin *j = FindJoin(back, alice->id);
	Check(j != nullptr, "alice is in bob's backfill");
	if (!j)
		return;

	Check(j->hdr.opcode == OP_S_PLAYER_JOIN && j->hdr.sendTimeMs == 1234,
	      "as a real S_PlayerJoin");
	Check(std::string(j->nick) == "alice" && j->netId == alice->netId,
	      "with her identity");
	Check(j->health == 37.0f && j->armour == 50.0f && j->weapon == 6,
	      "and the condition she is actually in");
	Check(j->pos.x == 100.0f && (j->flags & PJF_POS_VALID) != 0,
	      "at the position the session last heard, marked real");
	Check((j->flags & PJF_DEAD) == 0, "and alive");
}

// A joiner has to be told which doors are currently open for somebody.
//
// The garage mask is a *level* and it is only sent when it changes, so
// without this a joiner hears nothing until the door closes - and is then
// told a door they never saw open has shut. That is the whole reason the
// session remembers a mask at all; it arbitrates nothing here, because a
// garage belongs to the map and there is no owner to check a report against.
void TestAJoinerIsToldWhichDoorsAreOpen() {
	std::printf("\njoining while somebody is standing in their safehouse\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *carl  = Join(s, 2, "carl");

	{
		const Backfill back = s.BuildBackfill(carl->id, 1);
		Check(back.garages.empty(),
		      "nobody is in a garage, so nothing is said - which is the "
		      "overwhelmingly common case and why this is not a field on the "
		      "join packet");
	}

	alice->garageMask = 1u << 16;
	Player *bob = Join(s, 3, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 4321);
	Check(back.garages.size() == 1, "exactly one player has a door off its rest");
	if (back.garages.empty())
		return;
	Check(back.garages[0].hdr.opcode == OP_S_GARAGE_STATE &&
	          back.garages[0].hdr.sendTimeMs == 4321,
	      "as a real S_GarageState");
	Check(back.garages[0].playerId == alice->id, "tagged with whose it is");
	Check(back.garages[0].body.deviating == (1u << 16),
	      "and carrying the mask, not a door height - the door's position is "
	      "derived on every machine from the state");

	// And the joiner is never in their own backfill, doors included: a mask
	// handed back to its own author is an opinion a machine would OR into the
	// union it holds its own doors to.
	carl->garageMask = 1u << 2;
	const Backfill own = s.BuildBackfill(carl->id, 1);
	for (const S_GarageState &g : own.garages)
		Check(g.playerId != carl->id, "carl is not told about his own doors");
}

void TestAPlayerNobodyHasHeardFromHasNoPosition() {
	std::printf("\na player who has not sent a snapshot yet\n");
	Session s;
	Join(s, 1, "alice");
	Player *bob = Join(s, 2, "bob");

	// A Player starts at the origin because a struct has to start somewhere,
	// and the origin in GTA III is the water off Portland. Announcing it as a
	// position is how you get a remote ped that drowns in eight frames, which
	// this project has already paid for once.
	const S_PlayerJoin *j = FindJoin(s.BuildBackfill(bob->id, 1), 0);
	Check(j != nullptr && (j->flags & PJF_POS_VALID) == 0,
	      "the position bit is clear, so nobody creates a ped in the sea");

	Player *alice = s.FindById(0);
	s.NotePlayerState(*alice, State(5.0f, 100.0f));
	j = FindJoin(s.BuildBackfill(bob->id, 1), 0);
	Check(j != nullptr && (j->flags & PJF_POS_VALID) != 0,
	      "and it is set the moment one arrives");
}

void TestAJoinerIsToldWhoIsDead() {
	std::printf("\njoining while somebody is lying in the road\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	s.NotePlayerState(*alice, State(100.0f, 0.0f));

	// A death is an event, and an event only reaches whoever was connected
	// when it happened. Without the session keeping it, the next player in is
	// the one machine that thinks the body is standing up.
	s.NotePlayerDied(*alice, /*deathAnimId=*/17);

	Player *bob = Join(s, 2, "bob");
	const S_PlayerJoin *j = FindJoin(s.BuildBackfill(bob->id, 1), alice->id);
	Check(j != nullptr && (j->flags & PJF_DEAD) != 0, "bob is told she is dead");
	Check(j != nullptr && j->deathAnimId == 17,
	      "with the animation her own engine chose, so she lies the way she fell");

	s.NotePlayerRespawned(*alice, Vec3{9.0f, 9.0f, 9.0f}, 0.5f);
	j = FindJoin(s.BuildBackfill(bob->id, 1), alice->id);
	Check(j != nullptr && (j->flags & PJF_DEAD) == 0, "and alive again after a respawn");
	Check(j != nullptr && j->health == 100.0f && j->armour == 0.0f,
	      "on what the hospital hands back");
	Check(j != nullptr && j->pos.x == 9.0f, "at the hospital, not where she died");
}

void TestADeadPlayerIsInNoSeat() {
	std::printf("\ndying in a car, as a joiner sees it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	Check(car != nullptr, "alice claims a car");

	Player *bob = Join(s, 2, "bob");
	Check(s.BuildBackfill(bob->id, 1).seats.size() == 1, "bob is told she is driving");

	// Every client that was connected took her out of the seat before killing
	// the ped (client.cpp, OnDeath). The session has to agree, or the next
	// joiner is the only machine in the session that puts a corpse behind a
	// wheel.
	s.NotePlayerDied(*alice, 17);
	Check(s.BuildBackfill(bob->id, 1).seats.empty(), "and not once she is dead");
	Check(car->driverPlayerId == INVALID_PLAYER, "the car has nobody in it");
	Check(s.BuildBackfill(bob->id, 1).vehicles.size() == 1,
	      "but the car is still there - somebody parked it, it did not vanish");
}

void TestABackfilledCarCarriesItsCondition() {
	std::printf("\nwhat a joiner is told about a car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);

	s.NoteVehicleState(VehState(car->netId, 250.0f, 410.0f, VEH_ENGINE_ON | VEH_SIREN));

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 7);
	Check(back.vehicles.size() == 1, "the car is in the backfill");
	if (back.vehicles.empty())
		return;

	const S_VehicleSpawn &v = back.vehicles[0];
	Check(v.modelId == 91 && v.colour1 == 3 && v.colour2 == 4, "with its identity");
	Check(v.pos.x == 250.0f, "where it is now, not where it was claimed");
	Check(v.health == 410.0f, "and the health it has actually got left");
	Check(v.flags == (VEH_ENGINE_ON | VEH_SIREN), "engine and siren as the driver has them");
}

void TestAWreckIsNotBackfilled() {
	std::printf("\na car that has been blown up\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;

	// Zero health is a number; VEH_WRECKED is a fact. An observer that writes
	// zero into m_fHealth gets a car that reads dead and behaves brand new,
	// which is exactly what the owner found: a destroyed car came back to a
	// late joiner intact enough to climb into and too dead to drive.
	s.NoteVehicleState(VehState(netId, 100.0f, 0.0f));
	Check(!s.FindVehicle(netId)->destroyed, "zero health on its own is not a wreck");

	s.NoteVehicleState(VehState(netId, 100.0f, 0.0f, VEH_WRECKED));
	Check(s.FindVehicle(netId)->destroyed, "the driver saying so is");

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 1);
	Check(back.vehicles.empty(), "and a joiner is not handed a car that is gone");
	Check(back.seats.empty(), "nor a seat in one");
	Check(alice->vehicleNetId == INVALID_NETID, "the driver is out of it");

	// The row stays, so a snapshot still in flight for the wreck cannot
	// register a second car under the same number.
	Check(s.FindVehicle(netId) != nullptr, "the netId is still spoken for");
}

void TestADriverLeavingReleasesTheirCar() {
	std::printf("\nthe driver disconnecting\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);

	s.RemovePeer(1);
	Check(car->driverPlayerId == INVALID_PLAYER,
	      "the car is parked, not owned by a slot number");

	// Slots are reused. A stale driverPlayerId would make whoever fills this
	// one the owner of a car they have never seen - the same mistake
	// PickHost's "still here?" test exists to avoid.
	Player *carol = Join(s, 3, "carol");
	Check(carol != nullptr && carol->id == 0, "carol lands in alice's old slot");
	Check(car->driverPlayerId == INVALID_PLAYER, "and does not inherit her car");
}

void TestAPassengerIsBackfilledInTheirOwnSeat() {
	std::printf("\nriding shotgun, as a joiner sees it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	Player *carol = Join(s, 3, "carol");
	const Backfill back = s.BuildBackfill(carol->id, 1);
	Check(back.seats.size() == 2, "carol is told about both of them");

	const S_EnterVehicle *a = FindSeat(back, alice->id);
	const S_EnterVehicle *b = FindSeat(back, bob->id);
	Check(a != nullptr && a->body.seat == 0, "alice is driving");
	// The whole point. A passenger's seat has one carrier on the wire, the
	// S_EnterVehicle that announced it, and an event only reaches whoever was
	// connected when it was sent. The session used to keep drivers only, so
	// this said 0 and carol was the one machine in the session with a
	// passenger behind the wheel.
	Check(b != nullptr && b->body.seat == 1, "and bob is in the passenger seat");
	Check(a != nullptr && b != nullptr && a->body.netId == b->body.netId,
	      "in the same car");
	Check(car->driverPlayerId == alice->id,
	      "a passenger getting in does not take the wheel");
}

void TestOnlyTheDriverMayReportTheCar() {
	std::printf("\nwho gets to say what shape a car is in\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	Check(s.MayReportVehicle(alice->id, car->netId), "the driver may");
	Check(!s.MayReportVehicle(bob->id, car->netId), "the passenger beside her may not");
	Check(!s.MayReportVehicle(alice->id, 999), "and nobody may for a car that is not there");

	// Before version 9 this was survivable: the worst a stray snapshot could
	// do was move a car. Now the same packet carries VEH_WRECKED, so a
	// permissive gate is a way for any player in the session to delete any
	// car from every future backfill.
	s.NoteExitVehicle(*alice, car->netId);
	Check(!s.MayReportVehicle(alice->id, car->netId),
	      "and not once she has got out of it");
}

void TestAPassengerGetsOutOfAWreck() {
	std::printf("\nblowing up a car with two people in it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	s.DestroyVehicle(car->netId);
	// CAutomobile::BlowUpCar flags the driver and all eight passengers, not
	// the driver alone. A session that only emptied the driver's seat would
	// hand the next joiner a passenger sitting in a car it never sent.
	Check(alice->vehicleNetId == INVALID_NETID, "the driver is out");
	Check(bob->vehicleNetId == INVALID_NETID, "and so is the passenger");
	Check(s.BuildBackfill(2, 1).seats.empty(), "so a joiner is offered no seat in it");
}

void TestSteppingStraightFromOneCarIntoAnother() {
	std::printf("\nstepping from one car straight into another\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *first  = Claim(s, *alice, 91);
	const uint16_t firstId = first->netId;

	Vehicle *second = s.AddVehicle(105, 1, 2, Vec3{50.0f, 0.0f, 1.0f},
	                               Quat{0.0f, 0.0f, 0.0f, 1.0f});
	// No exit in front of it. A client jacking a car it is standing next to
	// sends one enter and nothing else, so the session has to let go of the
	// old car itself or that car keeps a driver for the rest of the session -
	// and keeping one means every joiner is told to seat alice in two cars.
	s.NoteEnterVehicle(*alice, *second, /*seat=*/0);

	Check(s.FindVehicle(firstId)->driverPlayerId == INVALID_PLAYER,
	      "the first car is parked");
	Check(second->driverPlayerId == alice->id, "and she is driving the second");

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 1);
	Check(back.seats.size() == 1 && back.seats[0].body.netId == second->netId,
	      "a joiner is told about one seat, in the car she is actually in");
	Check(back.vehicles.size() == 2, "and about both cars");
}

void TestTheJoinerIsNotInTheirOwnBackfill() {
	std::printf("\nthe joiner themselves\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Claim(s, *bob, 91);

	const Backfill back = s.BuildBackfill(bob->id, 1);
	Check(back.players.size() == 1 && back.players[0].playerId == alice->id,
	      "bob is told about alice and not about himself");
	Check(back.seats.empty(), "and is not told to seat himself in his own car");
	Check(back.vehicles.size() == 1, "though the car itself is still his to hear about");
}

// ---- ambient peds (docs/population.md §3 step 2) --------------------------

AmbientPedBody PedBody(uint16_t modelId, uint8_t pedType, float x) {
	AmbientPedBody b{};
	b.modelId = modelId;
	b.pedType = pedType;
	b.pos     = {x, 60.0f, 2.0f};
	b.heading = 0.25f;
	return b;
}

void TestOnlyTheOwnerMayTakeAPedAway() {
	std::printf("\nwho gets to remove an ambient ped\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientPed *ped = s.AddPed(alice->id, PedBody(7, 4, 5.0f));
	Check(ped != nullptr && ped->netId != INVALID_NETID,
	      "alice's pedestrian gets a name from the session");

	const uint16_t netId = ped->netId;
	Check(!s.RemovePed(netId, bob->id),
	      "bob cannot delete it - losing your replica says nothing about the ped");
	Check(s.FindPed(netId) != nullptr, "so it is still there");
	Check(s.RemovePed(netId, alice->id), "its owner can");
	Check(s.FindPed(netId) == nullptr, "and then it is gone");
}

void TestEveryPedGetsItsOwnName() {
	std::printf("\ntwo machines claiming at once\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	// The whole reason the handshake exists: both machines' generators run at
	// the same time and would pick the same temporary id. The session is what
	// makes those two different pedestrians.
	AmbientPed *a = s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	AmbientPed *b = s.AddPed(bob->id, PedBody(9, 5, 2.0f));
	Check(a && b && a->netId != b->netId, "two claims, two netIds");
	Check(a->ownerPlayerId == alice->id && b->ownerPlayerId == bob->id,
	      "each stays with the machine that made it");
}

void TestAPlayerTakesTheirPedsWithThem() {
	std::printf("\na player leaving\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	s.AddPed(alice->id, PedBody(7, 4, 2.0f));
	s.AddPed(bob->id, PedBody(9, 5, 3.0f));

	const std::vector<uint16_t> hers = s.PedsOwnedBy(alice->id);
	Check(hers.size() == 2, "alice is hosting two of them");
	for (uint16_t netId : hers)
		s.RemovePed(netId, INVALID_PLAYER);
	Check(s.PedsOwnedBy(alice->id).empty(), "and they go when she does");
	Check(s.PedsOwnedBy(bob->id).size() == 1, "bob keeps his");
}

void TestABackfilledPedIsNeverMistakenForYourOwn() {
	std::printf("\nbackfilling a crowd\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	s.AddPed(alice->id, PedBody(9, 5, 2.0f));
	Player *carol = Join(s, 3, "carol");
	s.AddPed(carol->id, PedBody(7, 4, 3.0f));

	const Backfill back = s.BuildBackfill(carol->id, 1);
	Check(back.peds.size() == 2, "carol hears about alice's two and not her own");
	for (const S_PedSpawn &spawn : back.peds) {
		// tempId 0 is the whole guard: a claim never uses it, so nothing in a
		// backfill can look like an answer to one.
		Check(spawn.tempId == 0, "a backfilled ped carries no temporary id");
		Check(spawn.ownerPlayerId == alice->id, "and says whose it is");
	}
}

void TestOnlyTheOwnerMayKillTheirPed() {
	std::printf("\nwho gets to say an ambient ped died\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientPed *ped = s.AddPed(alice->id, PedBody(7, 4, 5.0f));
	const uint16_t netId = ped->netId;

	PedDeathBody death{};
	death.netId  = netId;
	death.animId = 17;

	// An ambient ped's health lives on the machine hosting it and is not
	// even on the wire, so bob has nothing to base this on. Same rule as a
	// despawn: a claim about somebody else's ped is a statement about the
	// sender.
	Check(!s.NotePedDeath(death, bob->id), "bob cannot kill alice's pedestrian");
	Check(s.FindPed(netId)->alive, "so he is still on his feet");

	Check(s.NotePedDeath(death, alice->id), "its owner can");
	Check(!s.FindPed(netId)->alive && s.FindPed(netId)->deathAnimId == 17,
	      "and the session remembers how he fell");

	// Once per life. A second one is either a duplicate or a host that has
	// lost track, and relaying it runs CPed::SetDie again on every observer
	// over a state that is already the death's.
	Check(!s.NotePedDeath(death, alice->id), "and only once");

	// A death for a ped nobody has is not an error and is not relayed.
	PedDeathBody stranger{};
	stranger.netId = 9999;
	Check(!s.NotePedDeath(stranger, alice->id), "a ped the session never named is refused");
}

void TestAJoinerIsHandedTheCorpses() {
	std::printf("\na pedestrian who died before you connected\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	AmbientPed *walking = s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	AmbientPed *fallen  = s.AddPed(alice->id, PedBody(9, 5, 2.0f));
	const uint16_t deadNetId = fallen->netId;

	PedDeathBody death{};
	death.netId  = deadNetId;
	death.animId = 20;
	Check(s.NotePedDeath(death, alice->id), "alice's second pedestrian dies");

	Player *carol = Join(s, 3, "carol");
	const Backfill back = s.BuildBackfill(carol->id, 1);

	// Spawned and then killed, rather than left out the way a wrecked car
	// is. S_PedDeath can describe a corpse where S_CarSpawn cannot describe
	// a burnt shell, and a body everybody else walks around is not something
	// the joiner should be walking through.
	Check(back.peds.size() == 2, "both pedestrians are replayed");
	Check(back.pedDeaths.size() == 1, "and one of them is a body");
	Check(back.pedDeaths[0].body.netId == deadNetId,
	      "the one that died, not the one still walking");
	Check(back.pedDeaths[0].body.animId == 20,
	      "lying the way he fell rather than in the default knockdown");
	Check(walking->netId != deadNetId, "and the live one is untouched");
}

// ---- ambient traffic (docs/population.md §3 step 4) ----------------------
//
// The same four questions as the pedestrians, because the ownership rule is
// the same rule, plus the one a pedestrian never raised: a car moves, so the
// session has to keep its copy current or a latecomer is handed a car where
// it was born rather than where it is.

AmbientCarBody CarBody(uint16_t modelId, float x) {
	AmbientCarBody b{};
	b.modelId = modelId;
	b.colour1 = 3;
	b.colour2 = 9;
	b.extra1  = -1;
	b.extra2  = -1;
	b.pos     = {x, 60.0f, 2.0f};
	b.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	return b;
}

void TestOnlyTheOwnerMayTakeACarAway() {
	std::printf("\nwho gets to remove an ambient car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 5.0f));
	Check(car != nullptr && car->netId != INVALID_NETID,
	      "alice's traffic car gets a name from the session");

	const uint16_t netId = car->netId;
	Check(!s.RemoveCar(netId, bob->id),
	      "bob cannot delete it - losing your replica says nothing about the car");
	Check(s.FindCar(netId) != nullptr, "so it is still there");
	Check(s.RemoveCar(netId, alice->id), "its owner can");
	Check(s.FindCar(netId) == nullptr, "and then it is gone");
}

void TestEveryCarGetsItsOwnName() {
	std::printf("\ntwo machines making traffic at once\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientCar *a = s.AddCar(alice->id, CarBody(91, 1.0f));
	AmbientCar *b = s.AddCar(bob->id, CarBody(105, 2.0f));
	Check(a && b && a->netId != b->netId, "two claims, two netIds");
	Check(a->ownerPlayerId == alice->id && b->ownerPlayerId == bob->id,
	      "each stays with the machine that made it");

	// And not the same names the pedestrians got. One allocator, one namespace:
	// a netId means one entity in the session, whatever kind it is.
	AmbientPed *p = s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	Check(p && p->netId != a->netId && p->netId != b->netId,
	      "a ped and a car never share a netId");
}

void TestAPlayerTakesTheirCarsWithThem() {
	std::printf("\na player leaving takes their traffic\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	s.AddCar(alice->id, CarBody(91, 1.0f));
	s.AddCar(alice->id, CarBody(91, 2.0f));
	s.AddCar(bob->id, CarBody(105, 3.0f));

	const std::vector<uint16_t> hers = s.CarsOwnedBy(alice->id);
	Check(hers.size() == 2, "alice is hosting two of them");
	for (uint16_t netId : hers)
		s.RemoveCar(netId, INVALID_PLAYER);
	Check(s.CarsOwnedBy(alice->id).empty(), "and they go when she does");
	Check(s.CarsOwnedBy(bob->id).size() == 1, "bob keeps his");
}

void TestOnlyTheOwnerMovesTheirCar() {
	std::printf("\nwhose snapshot moves an ambient car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 5.0f));
	const uint16_t netId = car->netId;

	AmbientCarState moved{};
	moved.netId = netId;
	moved.pos   = {200.0f, 300.0f, 8.0f};
	moved.rot   = {0.0f, 0.0f, 0.0f, 1.0f};

	Check(!s.NoteCarState(moved, bob->id),
	      "bob cannot say where alice's car is");
	Check(s.FindCar(netId)->body.pos.x == 5.0f, "so it has not moved");
	Check(s.NoteCarState(moved, alice->id), "its owner can");
	Check(s.FindCar(netId)->body.pos.x == 200.0f, "and the session now agrees");

	// A snapshot for a car that has already gone is refused rather than
	// resurrecting a row. The despawn is reliable and the stream is not, so
	// this race happens every time a car is recycled.
	s.RemoveCar(netId, alice->id);
	Check(!s.NoteCarState(moved, alice->id),
	      "and a snapshot for a car that is gone brings nothing back");
}

void TestABackfilledCarIsWhereItIsNow() {
	std::printf("\nbackfilling traffic\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 1.0f));

	AmbientCarState moved{};
	moved.netId = car->netId;
	moved.pos   = {120.0f, 44.0f, 8.0f};
	moved.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	s.NoteCarState(moved, alice->id);

	Player *carol = Join(s, 3, "carol");
	s.AddCar(carol->id, CarBody(105, 3.0f));

	const Backfill back = s.BuildBackfill(carol->id, 1);
	Check(back.cars.size() == 1, "carol hears about alice's and not her own");
	Check(back.cars[0].tempId == 0, "a backfilled car carries no temporary id");
	Check(back.cars[0].ownerPlayerId == alice->id, "and says whose it is");
	// The whole point of Session::NoteCarState. Without it this reads 1.0f and
	// carol builds a car in a junction it drove out of minutes ago.
	Check(back.cars[0].body.pos.x == 120.0f,
	      "at the position the session last heard, not the one it was born at");
	Check(back.cars[0].body.colour1 == 3 && back.cars[0].body.colour2 == 9,
	      "with the colours its owner's engine rolled");
}

// ---- pickups ---------------------------------------------------------------
//
// The server is the only machine that sees two claims for the same pickup, so
// it is the only one that can say which of them gets it. docs/pickups.md 4.
//
// The thing most worth pinning down here is the split between a *reservation*
// and a *collection*. Claiming happens on approach, at 4 m, which is what
// removes the race - and it means players constantly claim things they turn
// out not to want. If a claim removed the pickup from everybody else's world,
// walking down a street would delete it.

PickupIdent Ident(float x, uint8_t type, int16_t model = 100,
                  uint8_t flags = 0) {
	PickupIdent id{};
	id.pos        = {x, 50.0f, 10.0f};
	id.modelIndex = model;
	id.type       = type;
	id.flags      = flags;
	return id;
}

void TestFirstClaimWins() {
	std::printf("first claim wins\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");
	const PickupIdent id = Ident(10.0f, 3 /* PICKUP_ONCE */);

	Check(s.ClaimPickup(a->id, id, 1000) == Session::PickupVerdict::GRANTED,
	      "the first claim is granted");
	Check(s.ClaimPickup(b->id, id, 1001) == Session::PickupVerdict::DENIED,
	      "the second claim a millisecond later is denied");
	Check(s.TakenPickups().size() == 1, "one record, not two");
	Check(s.TakenPickups()[0].byPlayerId == a->id, "and it has A's name on it");
	Check(s.TakenPickups()[0].state == TakenPickup::State::RESERVED,
	      "reserved, not taken - nobody has picked anything up yet");
}

void TestAReservationIsNotACollection() {
	std::printf("a reservation is not a collection\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");
	const PickupIdent id = Ident(15.0f, 2 /* ON_STREET */);

	s.ClaimPickup(a->id, id, 0);
	const Backfill back = s.BuildBackfill(b->id, 0);
	Check(back.pickups.empty(),
	      "a joiner is not told to remove something nobody has taken");

	// A walks away without collecting: the pickup is free again at once, and
	// it was never removed from anybody's world.
	s.ReleasePickup(id);
	Check(s.ClaimPickup(b->id, id, 100) == Session::PickupVerdict::GRANTED,
	      "and B can have it the moment A gives it back");
}

void TestOnlyTheHolderCanReportACollection() {
	std::printf("only the holder's own report turns a reservation into a removal\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");
	const PickupIdent id = Ident(18.0f, 3);

	s.ClaimPickup(a->id, id, 0);
	Check(!s.NotePickupCollected(b->id, id, 100),
	      "B cannot report a collection of A's reservation");
	Check(s.TakenPickups()[0].state == TakenPickup::State::RESERVED,
	      "and the record is untouched");

	Check(s.NotePickupCollected(a->id, id, 100), "A can");
	Check(s.TakenPickups()[0].state == TakenPickup::State::TAKEN, "it is taken");
	Check(!s.NotePickupCollected(a->id, id, 200),
	      "and a duplicate report is dropped rather than broadcast again");
}

void TestTheSamePickupIsMatchedWithinTolerance() {
	std::printf("identity is a position and a model, within tolerance\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");

	Check(s.ClaimPickup(a->id, Ident(10.0f, 3), 0) ==
	          Session::PickupVerdict::GRANTED,
	      "A claims it");
	// 10 cm out: the same pickup, measured on a machine whose ground-Z landed
	// on a different bit.
	Check(s.ClaimPickup(b->id, Ident(10.1f, 3), 0) ==
	          Session::PickupVerdict::DENIED,
	      "10 cm away is the same pickup");
	// A metre out: a different pickup entirely.
	Check(s.ClaimPickup(b->id, Ident(11.0f, 3), 0) ==
	          Session::PickupVerdict::GRANTED,
	      "a metre away is a different one");
	// Same place, different model: also a different pickup.
	Check(s.ClaimPickup(b->id, Ident(10.0f, 3, /*model=*/101), 0) ==
	          Session::PickupVerdict::GRANTED,
	      "the same place with another model is a different one");
}

void TestARespawningPickupComesBackAndAOnceDoesNot() {
	std::printf("the server owns availability, and the window is the type's\n");
	Session s;
	Player *a = Join(s, 1, "A");

	// PICKUP_ON_STREET, 30 s. The window only starts when it is collected.
	const PickupIdent street = Ident(20.0f, 2);
	Check(s.ClaimPickup(a->id, street, 0) == Session::PickupVerdict::GRANTED,
	      "reserved");
	Check(s.NotePickupCollected(a->id, street, 0), "and taken");
	Check(s.ClaimPickup(a->id, street, 29999) == Session::PickupVerdict::DENIED,
	      "still gone a millisecond before the window closes");
	Check(s.ClaimPickup(a->id, street, 30000) == Session::PickupVerdict::GRANTED,
	      "available again exactly on it");
	Check(s.TakenPickups().size() == 1,
	      "and the record was reused rather than a second one added");

	// PICKUP_ONCE never comes back.
	const PickupIdent once = Ident(30.0f, 3);
	s.ClaimPickup(a->id, once, 0);
	Check(s.NotePickupCollected(a->id, once, 0), "a ONCE pickup is taken");
	Check(s.ClaimPickup(a->id, once, 3600u * 1000u) ==
	          Session::PickupVerdict::DENIED,
	      "and it is still gone an hour later");
}

void TestABribeGetsItsOwnWindow() {
	std::printf("a bribe's window is 300 s where the rest of SLOW is 720\n");
	Session s;
	Player *a = Join(s, 1, "A");

	const PickupIdent bribe =
	    Ident(40.0f, 15 /* ON_STREET_SLOW */, 100, PICKUP_F_BRIBE);
	const PickupIdent other = Ident(50.0f, 15);

	s.ClaimPickup(a->id, bribe, 0);
	s.ClaimPickup(a->id, other, 0);
	Check(s.NotePickupCollected(a->id, bribe, 0), "the bribe is taken");
	Check(s.NotePickupCollected(a->id, other, 0), "and so is the other one");
	Check(s.ClaimPickup(a->id, bribe, 300000) == Session::PickupVerdict::GRANTED,
	      "the bribe is back at 300 s");
	Check(s.ClaimPickup(a->id, other, 300000) == Session::PickupVerdict::DENIED,
	      "the other one is not");
	Check(s.ClaimPickup(a->id, other, 720000) == Session::PickupVerdict::GRANTED,
	      "it comes back at 720 s");
}

void TestExpiryDropsOnlyWhatCanComeBack() {
	std::printf("expiry never forgets a pickup that never returns\n");
	Session s;
	Player *a = Join(s, 1, "A");
	const PickupIdent street  = Ident(60.0f, 2 /* ON_STREET, 30 s */);
	const PickupIdent package = Ident(70.0f, 5 /* COLLECTABLE1, never */);
	s.ClaimPickup(a->id, street, 0);
	s.ClaimPickup(a->id, package, 0);
	s.NotePickupCollected(a->id, street, 0);
	s.NotePickupCollected(a->id, package, 0);
	Check(s.TakenPickups().size() == 2, "two records");

	s.ExpirePickups(29000);
	Check(s.TakenPickups().size() == 2, "nothing expires early");

	s.ExpirePickups(31000);
	Check(s.TakenPickups().size() == 1, "the street weapon's record is gone");
	Check(s.TakenPickups()[0].ident.type == 5,
	      "and the hidden package's stayed - a package the server forgot "
	      "would count twice for the group");
}

void TestAnAbandonedReservationExpires() {
	std::printf("a reservation nobody ever gives back expires on its own\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");
	const PickupIdent id = Ident(75.0f, 3);

	s.ClaimPickup(a->id, id, 0);
	s.ExpirePickups(PICKUP_RESERVATION_MS - 1);
	Check(s.TakenPickups().size() == 1, "it stands while the window is open");
	Check(s.ClaimPickup(b->id, id, PICKUP_RESERVATION_MS - 1) ==
	          Session::PickupVerdict::DENIED,
	      "and B cannot have it");

	s.ExpirePickups(PICKUP_RESERVATION_MS);
	Check(s.TakenPickups().empty(),
	      "a client that crashed holding one does not lock it forever");
}

void TestAReleaseLetsAReusedKeyBeClaimedAgain() {
	std::printf("a taken-record is a lock, not a tombstone\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");

	// A rampage: PICKUP_ONCE, and the script puts it back at the same
	// coordinate after two failures.
	const PickupIdent rampage = Ident(80.0f, 3);
	s.ClaimPickup(a->id, rampage, 0);
	Check(s.NotePickupCollected(a->id, rampage, 0), "A starts the rampage");
	Check(s.ClaimPickup(b->id, rampage, 1000) == Session::PickupVerdict::DENIED,
	      "B cannot start it again while the record stands");

	// B's engine has a live object there again, so B says so.
	s.ReleasePickup(rampage);
	Check(s.TakenPickups().empty(), "the record is gone");
	Check(s.ClaimPickup(b->id, rampage, 2000) == Session::PickupVerdict::GRANTED,
	      "and the re-created pickup can be claimed");
}

void TestAReleaseForAKeyNobodyHoldsIsHarmless() {
	std::printf("releasing something nobody has does nothing\n");
	Session s;
	Player *a = Join(s, 1, "A");
	s.ClaimPickup(a->id, Ident(90.0f, 3), 0);
	s.ReleasePickup(Ident(999.0f, 3));
	Check(s.TakenPickups().size() == 1, "the unrelated record is untouched");
}

void TestAJoinerIsToldWhichPickupsAreGone() {
	std::printf("a joiner is told which pickups are already gone\n");
	Session s;
	Player *a = Join(s, 1, "A");
	const PickupIdent package = Ident(100.0f, 5 /* a hidden package */);
	const PickupIdent once    = Ident(110.0f, 3);
	s.ClaimPickup(a->id, package, 0);
	s.ClaimPickup(a->id, once, 0);
	s.NotePickupCollected(a->id, package, 0);
	s.NotePickupCollected(a->id, once, 0);

	Player *b = Join(s, 2, "B");
	const Backfill back = s.BuildBackfill(b->id, 0);
	Check(back.pickups.size() == 2, "both are in the backfill");
	Check(back.pickups[0].playerId == a->id, "and they carry who took them");
	// Without this the joiner is the one player in the session who can still
	// see - and walk into - a package everybody else has collected.
}

void TestLeavingGivesBackReservationsAndNotCollections() {
	std::printf("leaving gives back what you reserved, not what you took\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");
	const PickupIdent taken    = Ident(120.0f, 3);
	const PickupIdent reserved = Ident(130.0f, 3);

	s.ClaimPickup(a->id, taken, 0);
	s.NotePickupCollected(a->id, taken, 0);
	s.ClaimPickup(a->id, reserved, 0);

	const uint8_t aId = a->id;
	s.ReleaseReservationsOf(aId);
	s.RemovePeer(1);

	Check(s.ClaimPickup(b->id, reserved, 1000) ==
	          Session::PickupVerdict::GRANTED,
	      "the one A was only standing near is free at once");
	Check(s.ClaimPickup(b->id, taken, 1000) == Session::PickupVerdict::DENIED,
	      "the one A actually collected stays collected");
}

// ---- cars nobody owns (docs/roadmap.md 5.8) --------------------------------

UnownedVehicleKey ParkedKey(uint16_t generatorIndex) {
	UnownedVehicleKey k{};
	k.kind = UNOWNED_PARKED;
	k.pad  = 0;
	k.id   = generatorIndex;
	return k;
}

void TestTheFirstReportOfAnUnownedCarWins() {
	std::printf("two machines reporting the same parked car is not a contest\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Player *b = Join(s, 2, "B");

	// The normal case, not a race: the explosion that did it was replayed on
	// both machines, so both engines destroyed the car and both said so.
	Check(s.NoteUnownedBlowUp(ParkedKey(412), a->id, 0),
	      "the first report is recorded and relayed");
	Check(!s.NoteUnownedBlowUp(ParkedKey(412), b->id, 30),
	      "the second is dropped rather than fanned out again");
	Check(s.UnownedWrecks().size() == 1, "and there is one record, not two");
	Check(s.UnownedWrecks()[0].byPlayer == a->id,
	      "credited to whoever got there first");
}

void TestAnUnownedKindThisBuildDoesNotSpeakIsRefused() {
	std::printf("a key kind this build does not speak is dropped, not stored\n");
	Session s;
	Player *a = Join(s, 1, "A");

	UnownedVehicleKey future = ParkedKey(7);
	future.kind              = 99;
	Check(!s.NoteUnownedBlowUp(future, a->id, 0),
	      "a kind from some later build is refused");
	Check(s.UnownedWrecks().empty(),
	      "storing it would mean backfilling a key nobody can resolve");

	// And the kinds do not collide on the id, which is what would happen if
	// the key were compared on its number alone.
	Check(s.NoteUnownedBlowUp(ParkedKey(7), a->id, 0),
	      "the parked car with the same number is still its own thing");
}

// ---- and the traffic half of the same thing --------------------------------

UnownedVehicleKey AmbientKey(uint16_t netId) {
	UnownedVehicleKey k{};
	k.kind = UNOWNED_AMBIENT;
	k.pad  = 0;
	k.id   = netId;
	return k;
}

void TestOnlyTheHostMayWriteOffItsOwnTraffic() {
	std::printf("a traffic car is written off by the machine that made it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint16_t netId = s.AddCar(alice->id, CarBody(91, 5.0f))->netId;

	// Bob holds a replica of it. His replica has its own health - nothing on
	// the ambient wire carries a condition - so his engine really can blow it
	// up while alice's original is still driving around. That is his opinion
	// about her car and the session does not take it.
	Check(!s.NoteUnownedBlowUp(AmbientKey(netId), bob->id, 0),
	      "an observer may not write off somebody else's traffic");
	Check(!s.FindCar(netId)->destroyed, "and the session does not agree");

	Check(s.NoteUnownedBlowUp(AmbientKey(netId), alice->id, 10),
	      "the machine hosting it may");
	Check(s.FindCar(netId)->destroyed, "and that is the record");

	Check(!s.NoteUnownedBlowUp(AmbientKey(netId), alice->id, 20),
	      "saying it twice is not relayed twice");
	Check(s.UnownedWrecks().empty(),
	      "and it lands on the car's own row, not in the parked-wreck table");
}

void TestAnUnknownTrafficCarIsNotInvented() {
	std::printf("a wreck report for traffic nobody has heard of is dropped\n");
	Session s;
	Player *a = Join(s, 1, "A");
	Check(!s.NoteUnownedBlowUp(AmbientKey(4242), a->id, 0),
	      "there is nothing to write off and no model to build one from");
}

void TestAJoinerIsNotHandedBurntOutTraffic() {
	std::printf("a joiner is not handed a traffic car that already burned\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	// By value: m_cars is a vector and the second AddCar can move the first.
	const uint16_t alive = s.AddCar(alice->id, CarBody(91, 1.0f))->netId;
	const uint16_t dead  = s.AddCar(alice->id, CarBody(105, 2.0f))->netId;
	s.NoteUnownedBlowUp(AmbientKey(dead), alice->id, 0);

	Player *b = Join(s, 2, "B");
	const Backfill back = s.BuildBackfill(b->id, 0);
	Check(back.cars.size() == 1, "only the one still on the road");
	Check(back.cars[0].netId == alive, "and it is the right one");
	// Nothing in S_CarSpawn can describe a burnt shell, and the host stops
	// streaming it within the minute the engine takes to reap it.
}

void TestAParkedSessionCarCanBeReportedByAnybody() {
	std::printf("a car somebody parked and walked away from\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	UnownedVehicleKey k{};
	k.kind = UNOWNED_SESSION;
	k.id   = car->netId;

	// While she is driving it, it is hers. C_VehicleBlowUp is how she says so
	// and it carries the transform her own physics chose, which this packet
	// does not have.
	Check(!s.NoteUnownedBlowUp(k, bob->id, 0),
	      "bob may not write off the car alice is driving");
	Check(!s.FindVehicle(car->netId)->destroyed, "and it is not written off");

	// She gets out. Now nobody owns it, and there is nobody left who could
	// ever report it - which is the whole of roadmap.md 5.8.
	s.NoteExitVehicle(*alice, car->netId);
	Check(s.NoteUnownedBlowUp(k, bob->id, 0),
	      "once she is out, whoever was standing next to it may");
	Check(s.FindVehicle(car->netId)->destroyed, "and the session agrees");

	// It lands in Vehicle::destroyed rather than in a second table, because
	// that is already the record and DestroyVehicle is already the one place
	// it is written.
	Check(s.UnownedWrecks().empty(), "no second record for a car that has a row");
	Check(!s.NoteUnownedBlowUp(k, alice->id, 10), "and a second report is dropped");
}

// ---- what shape a car is in (docs/cardamage.md) ----------------------------

VehicleDamageBody Dent(uint16_t netId, unsigned panel, uint8_t panelLevel,
                       unsigned door = 0, uint8_t doorLevel = 0) {
	VehicleDamageBody b{};
	b.netId = netId;
	SetPanelLevel(b.panels, panel, panelLevel);
	if (doorLevel)
		SetDoorLevel(b.doors, door, doorLevel);
	return b;
}

void TestTheSessionRemembersTheWorstAnybodySaw() {
	std::printf("\nmerging damage reports\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);

	VehicleDamageBody out{};
	Check(s.NoteVehicleDamage(Dent(car->netId, 0, 1), out),
	      "a first dent is news");
	Check(GetPanelLevel(out.panels, 0) == 1, "and the reply says what it now is");

	Check(!s.NoteVehicleDamage(Dent(car->netId, 0, 1), out),
	      "the same report again is not");
	Check(!s.NoteVehicleDamage(Dent(car->netId, 0, 0), out),
	      "and neither is a gentler one - nothing un-dents a car but a respray");

	Check(s.NoteVehicleDamage(Dent(car->netId, 0, 3), out), "a worse one is");
	Check(GetPanelLevel(out.panels, 0) == 3, "and it wins");

	// Different components accumulate rather than replace, which is what makes
	// the record the union of what everybody saw rather than the last thing
	// anybody said.
	Check(s.NoteVehicleDamage(Dent(car->netId, 4, 1, 2, 2), out),
	      "a different panel and a door are both news");
	Check(GetPanelLevel(out.panels, 0) == 3 && GetPanelLevel(out.panels, 4) == 1 &&
	          GetDoorLevel(out.doors, 2) == 2,
	      "and the record is the union, not the newest report");

	Check(!s.NoteVehicleDamage(Dent(999, 0, 3), out),
	      "a car the session has never heard of is not invented");
}

void TestAWreckTakesNoMoreDamageReports() {
	std::printf("\na wreck's shape is not up for discussion\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);

	VehicleDamageBody out{};
	Check(s.NoteVehicleDamage(Dent(car->netId, 1, 2), out), "a dent lands");
	s.DestroyVehicle(car->netId);

	// CDamageManager::FuckCarCompletely gave it six missing doors and a zeroed
	// panel word on every machine, inside the BlowUpCar they all ran. A
	// snapshot-era report arriving after that would have the session remember
	// a car less broken than the one everybody is looking at.
	Check(!s.NoteVehicleDamage(Dent(car->netId, 3, 3), out),
	      "and nothing after the blast does");
}

// The collision between the damage work and the garage work: the first left
// the spray shop out of scope because garages were unsynced, the second built
// it, and nothing told the session the dents had gone.
void TestARespraysClearTravelsAsADamageReport() {
	std::printf("\na repair is the one report that lowers a car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);

	VehicleDamageBody out{};
	Check(s.NoteVehicleDamage(Dent(car->netId, 0, 3, 1, 2), out),
	      "the car takes a dent and a door");

	VehicleDamageBody repair{};
	repair.netId  = car->netId;
	repair.panels = VEH_DAMAGE_RESET;
	Check(s.NoteVehicleDamage(repair, out), "a repair is news");
	Check(IsDamageReset(out.panels),
	      "and it is relayed as a repair, not as a word the join would swallow");

	Player *bob = Join(s, 2, "bob");
	Check(s.BuildBackfill(bob->id, 1).vehicleDamage.empty(),
	      "so a joiner is handed no dents at all");

	// And the old high-water mark is really gone, rather than merely not being
	// backfilled: a gentler dent than the one sprayed off has to be news again.
	Check(s.NoteVehicleDamage(Dent(car->netId, 0, 1), out),
	      "a scrape lighter than the one that was sprayed off is news again");
	Check(GetPanelLevel(out.panels, 0) == 1, "and it is what the session holds");

	// A repair of an already-clean car is still relayed, because the marker is
	// what tells every observer's row to let go of what it is holding and the
	// server does not know what that is.
	VehicleDamageBody again{};
	again.netId  = car->netId;
	again.panels = VEH_DAMAGE_RESET;
	Check(s.NoteVehicleDamage(again, out), "a second repair still travels");
	Check(s.NoteVehicleDamage(again, out),
	      "and it is never refused as a restatement");
}

void TestAJoinerIsToldWhatShapeTheCarsAreIn() {
	std::printf("\nbackfilling a dented car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	// By value, not by pointer: Session keeps its cars in a vector and the
	// second Claim can move the first one. (That is how this test first
	// failed, and it is worth leaving the note - every other test here holds
	// one row at a time and never noticed.)
	const uint16_t hers = Claim(s, *alice, 91)->netId;
	const uint16_t mint = Claim(s, *alice, 105)->netId;

	VehicleDamageBody out{};
	s.NoteVehicleDamage(Dent(hers, 2, 3, 1, 2), out);

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 1);

	Check(back.vehicles.size() == 2, "bob hears about both cars");
	Check(back.vehicleDamage.size() == 1,
	      "and about the shape of the one that has been through something");
	if (back.vehicleDamage.size() == 1) {
		Check(back.vehicleDamage[0].body.netId == hers, "the right one");
		Check(GetPanelLevel(back.vehicleDamage[0].body.panels, 2) == 3 &&
		          GetDoorLevel(back.vehicleDamage[0].body.doors, 1) == 2,
		      "carrying what the session believes rather than any one report");
		Check(back.vehicleDamage[0].playerId == INVALID_PLAYER,
		      "as the session's own record, not as somebody's report");
	}
	(void)mint;
}

void TestAJoinerIsToldWhichParkedCarsAreWrecks() {
	std::printf("a joiner is told which parked cars are already burnt out\n");
	Session s;
	Player *a = Join(s, 1, "A");
	s.NoteUnownedBlowUp(ParkedKey(3), a->id, 0);
	s.NoteUnownedBlowUp(ParkedKey(90), a->id, 0);

	Player *b = Join(s, 2, "B");
	const Backfill back = s.BuildBackfill(b->id, 0);
	Check(back.unownedWrecks.size() == 2, "both are in the backfill");
	Check(back.unownedWrecks[0].key.kind == UNOWNED_PARKED,
	      "and they carry the key the receiver resolves");
	// This is the whole of 5.8: without it the joiner is handed a pristine
	// car standing where a shell is on every other screen.
}

void TestAnUnownedWreckStopsBeingBackfilledAfterAMinute() {
	std::printf("a wreck older than the engine's own removal is forgotten\n");
	Session s;
	Player *a = Join(s, 1, "A");
	s.NoteUnownedBlowUp(ParkedKey(11), a->id, 1000);

	s.ExpireUnownedWrecks(1000 + WRECK_BACKFILL_MS - 1);
	Check(s.UnownedWrecks().size() == 1, "still there a millisecond short");

	s.ExpireUnownedWrecks(1000 + WRECK_BACKFILL_MS);
	Check(s.UnownedWrecks().empty(), "and gone on the minute");

	// Not tidiness. CCarCtrl::PossiblyRemoveVehicle takes a wreck out of the
	// pool 60 seconds after m_nTimeOfDeath and the generator then parks a new
	// car in the space, so a record kept past that would have every future
	// joiner destroy a car nobody ever destroyed.
	Check(s.NoteUnownedBlowUp(ParkedKey(11), a->id, 90000),
	      "and the same space can be reported again once it has a new car");
}

// ---- ammunition (docs/protocol.md 1.9.6) ---------------------------------

AmmoSlotBody Ammo(uint8_t weapon, uint16_t clip, uint32_t total) {
	AmmoSlotBody a{};
	a.weapon = weapon;
	a.flags  = AMMO_SLOT_OWNED;
	a.clip   = clip;
	a.total  = total;
	return a;
}

// The same slot, as a player who has just put that weapon down reports it.
AmmoSlotBody NoAmmo(uint8_t weapon) {
	AmmoSlotBody a{};
	a.weapon = weapon;
	return a;
}

void TestAmmoSyncIsOffUntilTheServerSaysOtherwise() {
	std::printf("ammo sync is a server decision\n");
	Session s;
	Check(!s.AmmoSync(), "off by default, so a session behaves the way it always has");
	s.SetAmmoSync(true);
	Check(s.AmmoSync(), "and on once the server says so");
}

void TestOnlyAChangeIsWorthRelaying() {
	std::printf("a restated count is not news\n");
	Session s;
	Player *p = Join(s, 1, "ana");

	Check(s.NoteAmmo(*p, Ammo(2, 12, 96)), "the first thing said about a slot is news");
	Check(!s.NoteAmmo(*p, Ammo(2, 12, 96)), "saying it again is not");
	Check(s.NoteAmmo(*p, Ammo(2, 11, 95)), "one round fewer is");
	Check(!s.NoteAmmo(*p, Ammo(13, 1, 1)),
	      "and a slot number that is not one of the thirteen weapons is refused");
	Check(!s.NoteAmmo(*p, Ammo(200, 1, 1)), "including a wildly out of range one");
}

void TestAJoinerIsToldWhatEveryoneIsCarrying() {
	std::printf("a joiner is told what is in everyone's pockets\n");
	Session s;
	s.SetAmmoSync(true);
	Player *ana = Join(s, 1, "ana");
	s.NoteAmmo(*ana, Ammo(4, 8, 40));    // shotgun
	s.NoteAmmo(*ana, Ammo(2, 17, 102));  // pistol

	Player *bo = Join(s, 2, "bo");
	const Backfill back = s.BuildBackfill(bo->id, 1000);
	Check(back.ammo.size() == 2, "both of ana's slots are in the backfill");

	bool shotgun = false, pistol = false;
	for (const S_PlayerAmmo &a : back.ammo) {
		Check(a.playerId == ana->id, "and they are ana's");
		if (a.slot.weapon == 4)
			shotgun = a.slot.clip == 8 && a.slot.total == 40;
		if (a.slot.weapon == 2)
			pistol = a.slot.clip == 17 && a.slot.total == 102;
	}
	Check(shotgun, "the shotgun's numbers survived");
	Check(pistol, "and the pistol's");

	// A player is never handed their own inventory back - their own engine
	// is where it came from.
	s.NoteAmmo(*bo, Ammo(5, 1, 3));
	const Backfill mine = s.BuildBackfill(bo->id, 1000);
	for (const S_PlayerAmmo &a : mine.ammo)
		Check(a.playerId != bo->id, "the joiner is not in their own ammo backfill");
}

void TestLosingAWeaponIsNewsAndThenForgotten() {
	std::printf("losing a weapon is news, and then it is not carried\n");
	Session s;
	s.SetAmmoSync(true);
	Player *ana = Join(s, 1, "ana");
	Check(s.NoteAmmo(*ana, Ammo(4, 8, 40)), "she picks up a shotgun");
	Check(s.NoteAmmo(*ana, NoAmmo(4)), "and putting it down is news too");
	Check(!s.NoteAmmo(*ana, NoAmmo(4)), "saying so twice is not");

	// A joiner is told nothing about a slot nobody has - which is already
	// what an empty roster entry means on the far side.
	Player *bo = Join(s, 2, "bo");
	Check(s.BuildBackfill(bo->id, 1000).ammo.empty(),
	      "and the joiner is not sent an absence");
}

void TestNothingIsBackfilledWithTheSwitchOff() {
	std::printf("with ammo sync off the backfill says nothing about ammunition\n");
	Session s;
	Player *ana = Join(s, 1, "ana");
	// The record still gets kept - a server whose operator turns the option
	// on mid-session should not have to wait for everyone to reconnect.
	Check(s.NoteAmmo(*ana, Ammo(2, 17, 102)), "the count is still written down");

	Player *bo = Join(s, 2, "bo");
	Check(s.BuildBackfill(bo->id, 1000).ammo.empty(), "but nothing is handed over");

	s.SetAmmoSync(true);
	Check(!s.BuildBackfill(bo->id, 1000).ammo.empty(),
	      "and it is there the moment the option goes on");
}

} // namespace

int main() {
	TestHostIsTheFirstPlayerIn();
	TestAJoinerNeverTakesTheHostFromSomeoneStillHere();
	TestTheLastPlayerOutTakesItWithThem();
	TestTheClockAcceptsOnlyRealTimes();
	TestTheClockRollsOver();
	TestWeatherIsAPairAndIsChecked();

	TestABackfilledPlayerCarriesTheirCondition();
	TestAPlayerNobodyHasHeardFromHasNoPosition();
	TestAJoinerIsToldWhichDoorsAreOpen();
	TestAJoinerIsToldWhoIsDead();
	TestADeadPlayerIsInNoSeat();
	TestABackfilledCarCarriesItsCondition();
	TestAWreckIsNotBackfilled();
	TestADriverLeavingReleasesTheirCar();
	TestAPassengerIsBackfilledInTheirOwnSeat();
	TestOnlyTheDriverMayReportTheCar();
	TestAPassengerGetsOutOfAWreck();
	TestSteppingStraightFromOneCarIntoAnother();
	TestTheJoinerIsNotInTheirOwnBackfill();

	TestOnlyTheOwnerMayTakeAPedAway();
	TestEveryPedGetsItsOwnName();
	TestAPlayerTakesTheirPedsWithThem();
	TestABackfilledPedIsNeverMistakenForYourOwn();
	TestOnlyTheOwnerMayKillTheirPed();
	TestAJoinerIsHandedTheCorpses();
	TestOnlyTheOwnerMayTakeACarAway();
	TestEveryCarGetsItsOwnName();
	TestAPlayerTakesTheirCarsWithThem();
	TestOnlyTheOwnerMovesTheirCar();
	TestABackfilledCarIsWhereItIsNow();
	TestFirstClaimWins();
	TestAReservationIsNotACollection();
	TestOnlyTheHolderCanReportACollection();
	TestTheSamePickupIsMatchedWithinTolerance();
	TestARespawningPickupComesBackAndAOnceDoesNot();
	TestABribeGetsItsOwnWindow();
	TestExpiryDropsOnlyWhatCanComeBack();
	TestAnAbandonedReservationExpires();
	TestAReleaseLetsAReusedKeyBeClaimedAgain();
	TestAReleaseForAKeyNobodyHoldsIsHarmless();
	TestAJoinerIsToldWhichPickupsAreGone();
	TestLeavingGivesBackReservationsAndNotCollections();

	TestTheFirstReportOfAnUnownedCarWins();
	TestAnUnownedKindThisBuildDoesNotSpeakIsRefused();
	TestOnlyTheHostMayWriteOffItsOwnTraffic();
	TestAnUnknownTrafficCarIsNotInvented();
	TestAJoinerIsNotHandedBurntOutTraffic();
	TestAParkedSessionCarCanBeReportedByAnybody();
	TestTheSessionRemembersTheWorstAnybodySaw();
	TestAWreckTakesNoMoreDamageReports();
	TestARespraysClearTravelsAsADamageReport();
	TestAJoinerIsToldWhatShapeTheCarsAreIn();
	TestAJoinerIsToldWhichParkedCarsAreWrecks();
	TestAnUnownedWreckStopsBeingBackfilledAfterAMinute();

	TestAmmoSyncIsOffUntilTheServerSaysOtherwise();
	TestOnlyAChangeIsWorthRelaying();
	TestAJoinerIsToldWhatEveryoneIsCarrying();
	TestLosingAWeaponIsNewsAndThenForgotten();
	TestNothingIsBackfilledWithTheSwitchOff();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
