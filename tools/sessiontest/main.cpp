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

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
