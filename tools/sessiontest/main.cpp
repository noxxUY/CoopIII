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

#include "reach.h"
#include "session.h"

#include "coopiii/net.h"

#include <cstdio>
#include <cstring>
#include <limits>
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

// Claude's clothes are sent on change, so a joiner is only told them if the
// session wrote them down.
void TestAJoinerIsToldWhatEverybodyIsWearing() {
	std::printf("\nwhat a player is wearing, for whoever joins later\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	char look[PLAYER_LOOK_LEN] = "PLAYERP";
	Check(s.NotePlayerLook(*alice, look), "a new look is worth relaying");
	Check(std::string(alice->look) == "playerp", "and is kept cleaned");
	Check(!s.NotePlayerLook(*alice, look), "the same one again is not");
	char junk[PLAYER_LOOK_LEN] = "bad name";
	Check(!s.NotePlayerLook(*alice, junk) && std::string(alice->look) == "playerp",
	      "a name no engine would take is dropped");

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 77);
	Check(back.looks.size() == 1, "bob is told alice's look");
	if (!back.looks.empty())
		Check(back.looks[0].hdr.opcode == OP_S_PLAYER_LOOK &&
		          back.looks[0].hdr.sendTimeMs == 77 &&
		          back.looks[0].playerId == alice->id &&
		          std::string(back.looks[0].look) == "playerp",
		      "as a real S_PlayerLook, tagged with whose it is");
	Check(s.BuildBackfill(alice->id, 1).looks.empty(),
	      "and nobody is told a look that was never said");

	s.RemovePeer(1);
	Player *carl = Join(s, 3, "carl");
	Check(carl->look[0] == '\0', "a reused slot starts with no look");
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

// Busted. The client sends the police station as a C_Respawn with no C_Death
// in front of it, because nobody died - the ped was arrested and moved. The
// server has to take that as it is: still alive, somewhere else, out of the
// car the cop caught them in.
void TestAnArrestIsARespawnWithoutADeath() {
	std::printf("\nleaving the police station, as a joiner sees it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NotePlayerState(*alice, State(100.0f, 80.0f, 50.0f));

	s.NotePlayerRespawned(*alice, Vec3{1200.0f, -300.0f, 10.0f}, 1.5f);
	Check(alice->alive, "never dead, still alive");

	Player *bob = Join(s, 2, "bob");
	const Backfill      fill = s.BuildBackfill(bob->id, 1);
	const S_PlayerJoin *j    = FindJoin(fill, alice->id);
	Check(j != nullptr && (j->flags & PJF_DEAD) == 0, "bob is not told she is dead");
	Check(j != nullptr && j->pos.x == 1200.0f, "he is told she is at the police station");
	Check(j != nullptr && j->health == 100.0f && j->armour == 0.0f,
	      "on what the station hands back");
	Check(fill.seats.empty() && car->driverPlayerId == INVALID_PLAYER,
	      "and not in the car she was arrested in");
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
	//
	// Getting out no longer ends her right to report it *immediately*, and
	// that is the custody change rather than a hole: she is the one machine
	// asked to finish whatever the car was doing, because she was driving it
	// a frame ago and is standing next to it. What has not changed is that
	// nobody else may, and that the right ends the moment she says the car
	// has settled.
	s.NoteExitVehicle(*alice, car->netId);
	Check(s.MayReportVehicle(alice->id, car->netId),
	      "once she is out she is the one settling it, so she still may");
	Check(!s.MayReportVehicle(bob->id, car->netId),
	      "and the passenger still may not");
	Check(s.EndCustody(car->netId, alice->id),
	      "she can hand it back when it has come to rest");
	Check(!s.MayReportVehicle(alice->id, car->netId),
	      "and then she may not either - nobody simulates it now");
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

// ---- the carjack -----------------------------------------------------------
//
// The one decision in this whole area that only the server can make.
//
// A carjack happens in exactly one process: the jacker's engine plays the
// animation, drags the victim's replica out of the seat and puts its own player
// behind the wheel. The victim's engine is never told anything and still has
// *its* player behind the wheel. Both clients then answer "we drive that car"
// to CVehicle::m_pDriver, which is the only question either of them can ask,
// and neither is wrong from where it is standing. Nothing a client can look at
// breaks that tie.
//
// The session's record does. Before this, it broke it badly: the name in
// driverPlayerId was simply overwritten, the player the car had been taken from
// was never told, and the record kept them in seat 0 of a car it now said
// somebody else was driving. Their machine went on believing it owned the car
// for the rest of the session - refusing every snapshot the new owner sent, so
// the car stood still on that screen while it was driven away on the other -
// and its own snapshots were dropped by MayReportVehicle, so nothing it did
// with the car reached anybody either.
void TestAJackTakesTheCarOffThePlayerWhoHadIt() {
	std::printf("\none car, two players who both think they are driving it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	Vehicle       *car   = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	Check(car->driverPlayerId == alice->id, "alice is at the wheel");
	Check(s.MayReportVehicle(alice->id, netId), "so the session believes her snapshots");

	const uint8_t displaced = s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
	Check(displaced == alice->id,
	      "bob taking the wheel says who he took it from - which is the only "
	      "way she can ever be told");
	Check(car->driverPlayerId == bob->id, "the car is his now");
	Check(alice->vehicleNetId == INVALID_NETID,
	      "and she is recorded out of it, so no joiner is told two people are "
	      "in one seat");
	Check(!s.MayReportVehicle(alice->id, netId), "her snapshots stop being believed");
	Check(s.MayReportVehicle(bob->id, netId), "and his start");

	// A joiner arriving now is told about one seat, bob's.
	Player       *carol = Join(s, 3, "carol");
	const Backfill back = s.BuildBackfill(carol->id, 1);
	Check(back.seats.size() == 1 && back.seats[0].playerId == bob->id,
	      "and a joiner is told about one driver, not two");
}

void TestTakingAnEmptySeatIsNotAJack() {
	std::printf("\nwhat is not a jack\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	Vehicle *parked = s.AddVehicle(91, 1, 2, Vec3{1.0f, 1.0f, 1.0f},
	                              Quat{0.0f, 0.0f, 0.0f, 1.0f});
	Check(s.NoteEnterVehicle(*bob, *parked, /*seat=*/0) == INVALID_PLAYER,
	      "a parked car had nobody to take it from");
	Check(s.NoteEnterVehicle(*bob, *parked, /*seat=*/0) == INVALID_PLAYER,
	      "and a repeat of the same claim does not displace the claimer");

	// A passenger climbing in does not take the car off its driver. That is the
	// whole reason the test is on seat 0 rather than on "somebody got in".
	Vehicle *hers = Claim(s, *alice, 105);
	Check(s.NoteEnterVehicle(*bob, *hers, /*seat=*/2) == INVALID_PLAYER,
	      "and riding in the back is not a jack");
	Check(hers->driverPlayerId == alice->id, "alice keeps the wheel");
	Check(s.MayReportVehicle(alice->id, hers->netId), "and keeps reporting it");
}

void TestAJackedCarKeepsItsPassengers() {
	std::printf("\njacking a car with somebody in the back\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");

	Vehicle *car = Claim(s, *alice, 91);
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	// Carol jacks alice. Bob is in the back and stays there, exactly as the
	// engine's own jack leaves him: only the driver's seat carries ownership
	// and only the driver's seat changes hands.
	Check(s.NoteEnterVehicle(*carol, *car, /*seat=*/0) == alice->id,
	      "carol took it off alice");
	Check(bob->vehicleNetId == car->netId && bob->seat == 1,
	      "and bob is still sitting in the back of it");
	Check(!s.MayReportVehicle(bob->id, car->netId),
	      "a passenger still speaks for nothing");
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
	// makes those two different pedestrians. Kept as netIds, because the rows
	// live in a vector and the second claim can move the first.
	const uint16_t a = s.AddPed(alice->id, PedBody(7, 4, 1.0f))->netId;
	const uint16_t b = s.AddPed(bob->id, PedBody(9, 5, 2.0f))->netId;
	Check(a != INVALID_NETID && b != INVALID_NETID && a != b, "two claims, two netIds");
	Check(s.FindPed(a)->ownerPlayerId == alice->id && s.FindPed(b)->ownerPlayerId == bob->id,
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

void TestOnlySomebodyElseMayShootYourPed() {
	std::printf("\nwho gets to say an ambient ped was shot\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientPed *ped = s.AddPed(alice->id, PedBody(7, 4, 5.0f));
	const uint16_t netId = ped->netId;

	// Every other ambient claim - a despawn, a limb, a death - is refused to
	// anybody but the owner. This one is the only packet that goes the other
	// way, so the rule is inverted, and the inversion is the point: a hit is
	// resolved on the shooter's machine from a ray only the shooter has.
	Player *to = s.PedDamageRecipient(netId, bob->id);
	Check(to == alice, "bob's hit on alice's pedestrian is routed to alice");
	Check(s.PedDamageRecipient(netId, alice->id) == nullptr,
	      "and alice cannot report a hit on her own - her engine already applied it");

	// Point to point. Nobody else has anything to do with it: what the rest of
	// the session needs to see reaches them from alice afterwards.
	Player *carol = Join(s, 3, "carol");
	Check(s.PedDamageRecipient(netId, carol->id) == alice,
	      "carol's hit on the same pedestrian goes to alice too, and only to her");

	// A pedestrian the session has never had. Not an error - a despawn is
	// reliable and a hit races it - and not invented into existence either.
	Check(s.PedDamageRecipient(9999, bob->id) == nullptr,
	      "a pedestrian the session never named is refused");

	// Already on the floor. The owner's own CPed::InflictDamage refuses a ped
	// that is dying or dead at 0x004EA485, so this only saves the trip - but a
	// burst that was in flight when he dropped is the ordinary case, not a rare
	// one.
	PedDeathBody death{};
	death.netId  = netId;
	death.animId = 17;
	Check(s.NotePedDeath(death, alice->id), "he dies");
	Check(s.PedDamageRecipient(netId, bob->id) == nullptr,
	      "and the rest of the burst is not relayed onto the corpse");
}

void TestAPedestriansRoundsAndHitsAreHisHosts() {
	std::printf("\nwho gets to say a pedestrian fired, and who his hit goes to\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint8_t  aliceId  = alice->id;
	const uint8_t  bobId    = bob->id;
	const uint16_t bobNetId = bob->netId;

	const uint16_t cop = s.AddPed(aliceId, PedBody(7, 4, 5.0f))->netId;

	// The direction C_PedDamage does not have: from the pedestrian's host,
	// towards a player.
	Check(s.NpcShotAllowed(cop, aliceId), "alice's machine may say her pedestrian fired");
	Check(!s.NpcShotAllowed(cop, bobId),
	      "bob's may not - his copy of the cop decides nothing");
	Check(!s.NpcShotAllowed(9999, aliceId), "nor anybody, for a pedestrian never named");

	Check(s.NpcHitRecipient(cop, bobNetId, aliceId) == s.FindById(bobId),
	      "the cop's hit on bob goes to bob");
	Check(s.NpcHitRecipient(cop, bobNetId, bobId) == nullptr,
	      "and bob cannot claim alice's cop hit him - his engine has no cop to ask");
	Check(s.NpcHitRecipient(cop, alice->netId, aliceId) == nullptr,
	      "a hit on alice herself is her own engine's, and never travels");
	Check(s.NpcHitRecipient(cop, 9999, aliceId) == nullptr,
	      "nor does one on a player who is not here");

	s.NotePlayerDied(*s.FindById(bobId), 17);
	Check(s.NpcHitRecipient(cop, bobNetId, aliceId) == nullptr,
	      "the rest of the burst is not relayed onto bob's body");

	PedDeathBody death{};
	death.netId  = cop;
	death.animId = 17;
	Check(s.NotePedDeath(death, aliceId), "the cop dies");
	Check(!s.NpcShotAllowed(cop, aliceId), "and a corpse fires no more rounds");
}

void TestFriendlyFireHasNoSayOverPedestrians() {
	std::printf("\nfriendly fire is about players, not pedestrians\n");

	// The default session, which is the one the bug was reported on. Friendly
	// fire off stops one player hurting another (docs/roadmap.md §5.2) and must
	// not stop anybody shooting NPCs - a session where the whole city is
	// bulletproof is the bug, not the fix.
	Session off;
	Check(!off.FriendlyFire(), "off is the default");
	Player *a = Join(off, 1, "alice");
	Player *b = Join(off, 2, "bob");
	AmbientPed *ped = off.AddPed(a->id, PedBody(7, 4, 5.0f));
	Check(off.PedDamageRecipient(ped->netId, b->id) == a,
	      "and bob can still shoot alice's pedestrian");

	// And it makes no difference the other way either, which is what says the
	// flag is not consulted rather than merely happening to allow it.
	Session on;
	on.SetFriendlyFire(true);
	Player *c = Join(on, 1, "alice");
	Player *d = Join(on, 2, "bob");
	AmbientPed *p2 = on.AddPed(c->id, PedBody(7, 4, 5.0f));
	Check(on.PedDamageRecipient(p2->netId, d->id) == c,
	      "with it on, the same hit is routed the same way");
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

	// Netids for the reason TestEveryPedGetsItsOwnName keeps them.
	const uint16_t a = s.AddCar(alice->id, CarBody(91, 1.0f))->netId;
	const uint16_t b = s.AddCar(bob->id, CarBody(105, 2.0f))->netId;
	Check(a != INVALID_NETID && b != INVALID_NETID && a != b, "two claims, two netIds");
	Check(s.FindCar(a)->ownerPlayerId == alice->id && s.FindCar(b)->ownerPlayerId == bob->id,
	      "each stays with the machine that made it");

	// And not the same names the pedestrians got. One allocator, one namespace:
	// a netId means one entity in the session, whatever kind it is.
	AmbientPed *p = s.AddPed(alice->id, PedBody(7, 4, 1.0f));
	Check(p && p->netId != a && p->netId != b,
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

void TestAPackageIsEverybodysByDefault() {
	std::printf("a hidden package, shared\n");
	Session s;
	Player *a = Join(s, 1, "alice");
	Player *b = Join(s, 2, "bob");
	const PickupIdent pkg = Ident(1.0f, PICKUP_TYPE_PACKAGE, 1321);
	Check(s.PackageRule() == PACKAGES_SHARED, "the rule is shared unless the server says");
	Check(!s.PickupIsPerPlayer(pkg), "so a package is one lock for the session");
	s.ClaimPickup(a->id, pkg, 10);
	s.NotePickupCollected(a->id, pkg, 20);
	Check(s.ClaimPickup(b->id, pkg, 30) == Session::PickupVerdict::DENIED,
	      "alice took it, so bob cannot");
	Player *c = Join(s, 3, "carol");
	Check(s.BuildBackfill(c->id, 40).pickups.size() == 1, "and a joiner is told it is gone");
}

void TestAPackageIsYourOwnUnderPerPlayer() {
	std::printf("a hidden package, per player\n");
	Session s;
	s.SetPackageRule(PACKAGES_PERPLAYER);
	Player *a = Join(s, 1, "alice");
	Player *b = Join(s, 2, "bob");
	const PickupIdent pkg  = Ident(1.0f, PICKUP_TYPE_PACKAGE, 1321);
	const PickupIdent shot = Ident(9.0f, 2, 101);   // PICKUP_ON_STREET

	Check(s.PickupIsPerPlayer(pkg) && !s.PickupIsPerPlayer(shot),
	      "only the packages are per player");
	Check(s.ClaimPickup(a->id, pkg, 10) == Session::PickupVerdict::GRANTED &&
	          s.ClaimPickup(b->id, pkg, 11) == Session::PickupVerdict::GRANTED,
	      "two players on the same package are both granted it");
	Check(s.NotePickupCollected(a->id, pkg, 20) && s.NotePickupCollected(b->id, pkg, 21),
	      "and both collect their own");
	Check(s.ClaimPickup(a->id, pkg, 30) == Session::PickupVerdict::DENIED,
	      "but nobody collects theirs twice");

	s.ClaimPickup(a->id, shot, 40);
	Check(s.ClaimPickup(b->id, shot, 41) == Session::PickupVerdict::DENIED,
	      "a shotgun on the street is still first come, first served");

	Player *c = Join(s, 3, "carol");
	Check(s.BuildBackfill(c->id, 50).pickups.empty(),
	      "a joiner is handed nobody else's packages to lose");
	Check(s.ClaimPickup(c->id, pkg, 60) == Session::PickupVerdict::GRANTED,
	      "and finds this one where it always was");

	s.ReleasePickup(pkg, b->id);
	Check(s.ClaimPickup(b->id, pkg, 70) == Session::PickupVerdict::GRANTED &&
	          s.ClaimPickup(a->id, pkg, 71) == Session::PickupVerdict::DENIED,
	      "a release lets go of the sender's own record and nobody else's");

	// Alice goes and dave gets her slot: her packages are not his.
	const uint8_t aliceSlot = a->id;
	s.NotePickupCollected(b->id, pkg, 72);
	s.RemovePeer(1);
	Player *d = Join(s, 4, "dave");
	Check(d != nullptr && d->id == aliceSlot, "(dave is in alice's old slot)");
	Check(s.ClaimPickup(d->id, pkg, 80) == Session::PickupVerdict::GRANTED,
	      "a new player in an old slot finds every package for himself");
	Check(s.ClaimPickup(b->id, pkg, 81) == Session::PickupVerdict::DENIED,
	      "and the players still here keep what they found");
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

void TestACollectedDropIsForgotten() {
	std::printf("a collected drop is forgotten, a one-off pickup is not\n");
	Session s;
	Player *a = Join(s, 1, "A");
	const PickupIdent drop = Ident(80.0f, PICKUP_TYPE_MONEY);
	const PickupIdent once = Ident(81.0f, 3);
	s.ClaimPickup(a->id, drop, 0);
	s.ClaimPickup(a->id, once, 0);
	s.NotePickupCollected(a->id, drop, 0);
	s.NotePickupCollected(a->id, once, 0);
	s.ExpirePickups(PICKUP_DROP_FORGET_MS - 1);
	Check(s.TakenPickups().size() == 2, "both are remembered for a while");
	s.ExpirePickups(PICKUP_DROP_FORGET_MS);
	Check(s.TakenPickups().size() == 1 && s.TakenPickups()[0].ident.type == 3,
	      "then the money a ped dropped is forgotten and the one-off stays taken");
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
	Check(s.TakenPickups().size() == 1,
	      "past the window it still stands while A is here to honour it");
	s.ReleasePickup(id, b->id);
	Check(s.TakenPickups().size() == 1, "and B walking away cannot give A's back");

	s.RemovePeer(a->peer);
	s.ExpirePickups(PICKUP_RESERVATION_MS + 1);
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
	Check(back.pickups[0].playerId == INVALID_PLAYER,
	      "from nobody, so a joiner in the collector's old slot does not drop them");
	// Without this the joiner is the one player in the session who can still
	// see - and walk into - a package everybody else has collected.
	s.RemovePeer(a->peer);
	Player *again = Join(s, 3, "A again");
	const Backfill rejoin = s.BuildBackfill(again->id, 0);
	Check(again->id == a->id && rejoin.pickups.size() == 2 &&
	          rejoin.pickups[0].playerId != again->id,
	      "and the collector coming back into the same slot is told as well");
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

	// She gets out, and settles it. Now nobody owns it, and there is nobody
	// left who could ever report it - which is the whole of roadmap.md 5.8.
	s.NoteExitVehicle(*alice, car->netId);
	s.EndCustody(car->netId, alice->id);
	Check(s.NoteUnownedBlowUp(k, bob->id, 0),
	      "once she is out and it has settled, whoever was standing next to it may");
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

// ---- shooting somebody else's car (protocol.h, VehicleHitBody) -------------
//
// Every other packet about a claimed car is a statement about the sender's own
// world, and MayReportVehicle is the one rule that checks all of them. This one
// runs the other way, so its rule is the inverse - and the inverse is what
// these tests are about, because getting it the right way round is the whole
// difference between a working feature and a machine being talked into
// damaging a car it is only watching.
void TestAHitOnACarIsRoutedToItsDriver() {
	std::printf("\na hit on a car is routed to whoever is driving it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "bob's hit on alice's car goes to alice");
	Check(s.VehicleHitRecipient(car->netId, alice->id) == nullptr,
	      "and alice cannot report a hit on the car she is driving - her own "
	      "engine already applied it");

	// Point to point. Nobody else has anything to do with it: what the rest of
	// the session needs to see reaches them from alice afterwards, on the
	// snapshot, on C_VehicleDamage and on C_VehicleBlowUp.
	Player *carol = Join(s, 3, "carol");
	Check(s.VehicleHitRecipient(car->netId, carol->id) == alice,
	      "carol's hit on the same car goes to alice too, and only to her");

	// A car the session has never had. Not an error - a despawn is reliable and
	// a hit races it - and not invented into existence either.
	Check(s.VehicleHitRecipient(9999, bob->id) == nullptr,
	      "a car the session never named is refused");
}

void TestNobodyMayShootACarNobodyIsDriving() {
	std::printf("\na car with nobody in it has nobody to tell\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	// She parks it and walks away, and once it has settled nobody holds it.
	// VehicleHitRecipient has nobody to route a hit to - a bystander would be
	// taking health off a car it does not own either. The shooter gets it
	// instead, through CustodyForHit, which Server::OnVehicleHit asks first.
	s.NoteExitVehicle(*alice, car->netId);
	Check(s.EndCustody(car->netId, alice->id), "she settles it");
	Check(s.VehicleHitRecipient(car->netId, bob->id) == nullptr,
	      "an abandoned car has no recipient of its own");
	Check(s.VehicleHitRecipient(car->netId, alice->id) == nullptr,
	      "not even from the player who parked it");

	// And it starts taking them again the moment somebody is at the wheel,
	// which is what says the test is about the driver rather than about the
	// car having once had one.
	s.NoteEnterVehicle(*alice, *car, /*seat=*/0);
	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "and again as soon as she gets back in");
}

void TestAWreckedCarTakesNoMoreHits() {
	std::printf("\na burst still in the air when the car went up\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice, "it is hittable");
	s.DestroyVehicle(car->netId);

	// The owner's own CVehicle::InflictDamage would refuse it anyway - health
	// <= 0 leaves at 0x00551A10 before the arithmetic - so this only saves the
	// trip. A burst that was in flight when the car exploded is the ordinary
	// case, not a rare one.
	Check(s.VehicleHitRecipient(car->netId, bob->id) == nullptr,
	      "and the rest of the burst is not relayed onto the wreck");
}

void TestAPassengerMayNotShootTheCarHeIsSittingIn() {
	std::printf("\na passenger is not the driver, in either direction\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	// Bob is in the car and is still not its owner - the same rule
	// MayReportVehicle applies to his snapshots. So a hit he lands goes to
	// alice like anybody else's, rather than being refused as "his own car" or
	// routed to himself.
	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "a passenger's hit is still routed to the driver");
}

void TestFriendlyFireHasNoSayOverCars() {
	std::printf("\nfriendly fire is about players, not cars\n");

	// The default session. Friendly fire off stops one player hurting another
	// (docs/roadmap.md 5.2) and must not stop anybody shooting a car - a
	// session where every car is bulletproof is the bug, not the fix. It is
	// also not the lever anybody would want: with it off you can still ram the
	// same car off a bridge.
	Session off;
	Check(!off.FriendlyFire(), "off is the default");
	Player *a = Join(off, 1, "alice");
	Player *b = Join(off, 2, "bob");
	Vehicle *car = Claim(off, *a, 91);
	Check(off.VehicleHitRecipient(car->netId, b->id) == a,
	      "and bob can still shoot alice's car");

	// And it makes no difference the other way either, which is what says the
	// flag is not consulted rather than merely happening to allow it.
	Session on;
	on.SetFriendlyFire(true);
	Player *c = Join(on, 1, "alice");
	Player *d = Join(on, 2, "bob");
	Vehicle *car2 = Claim(on, *c, 91);
	Check(on.VehicleHitRecipient(car2->netId, d->id) == c,
	      "with it on, the same hit is routed the same way");
}

void TestAHitIsTheInverseOfEveryOtherCarPacket() {
	std::printf("\nthe one car packet whose ownership rule runs backwards\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	// Stated as the pair rather than as two separate facts, because the pair is
	// the invariant: for a given car and a given player, exactly one of "may I
	// describe it" and "should I be told about a hit on it" can be true, and
	// which one it is never depends on anything but the driver.
	Check(s.MayReportVehicle(alice->id, car->netId) &&
	          s.VehicleHitRecipient(car->netId, alice->id) == nullptr,
	      "the driver describes the car and is never told about hits on it");
	Check(!s.MayReportVehicle(bob->id, car->netId) &&
	          s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "everybody else lands hits on it and never describes it");

	// Nothing is recorded. A hit is not a state a joiner has to be handed - the
	// health it produced lives on the owner's machine and reaches the session
	// on the snapshot that has always carried it - so asking twice gives the
	// same answer rather than a smaller one.
	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice &&
	          s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "and the session keeps no record of one, so it does not run out");
}

// A car somebody is settling (S_VehicleCustody). The custodian's engine is the
// one simulating it and streaming its health, so a hit goes to them the way it
// goes to a driver - MayReportVehicle's precedence, turned round.
void TestAHitOnACarInCustodyGoesToTheCustodian() {
	std::printf("\na hit on a car somebody is settling goes to them\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);
	Check(s.CustodianOf(car->netId) == alice->id, "alice is settling it");

	Check(s.VehicleHitRecipient(car->netId, bob->id) == alice,
	      "bob's hit goes to alice, whose engine is the one simulating it");
	Check(s.VehicleHitRecipient(car->netId, alice->id) == nullptr,
	      "and alice's own is hers already - her engine applied it");
	Check(s.MayReportVehicle(alice->id, car->netId) &&
	          !s.MayReportVehicle(bob->id, car->netId),
	      "the same one machine that may describe it");

	// It settles. A hit fired during the settle that lands now has nobody to
	// go to - the car is nobody's, like any parked car.
	s.EndCustody(car->netId, alice->id);
	Check(s.VehicleHitRecipient(car->netId, bob->id) == nullptr,
	      "after the settle, a late hit is dropped");
}

void TestACustodyEndingMovesTheHitsOn() {
	std::printf("\nthe three other ways a custody ends, with a hit in flight\n");

	// Somebody gets in. The hit goes to the new driver.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		Player *bob   = Join(s, 2, "bob");
		Player *carol = Join(s, 3, "carol");
		Vehicle *car  = Claim(s, *alice, 91);
		s.NoteExitVehicle(*alice, car->netId);
		s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
		Check(s.VehicleHitRecipient(car->netId, carol->id) == bob,
		      "bob got in: carol's hit is his");
		Check(s.VehicleHitRecipient(car->netId, alice->id) == bob,
		      "and alice's too, now she isn't settling it");
	}

	// The custodian quits. Handed to nobody, so nobody takes it.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		Player *bob   = Join(s, 2, "bob");
		Vehicle *car  = Claim(s, *alice, 91);
		const uint16_t netId = car->netId;
		s.NoteExitVehicle(*alice, netId);
		s.RemovePeer(1);
		Check(s.CustodianOf(netId) == INVALID_PLAYER, "her custody went with her");
		Check(s.VehicleHitRecipient(netId, bob->id) == nullptr,
		      "and bob's hit has nobody to go to");
	}

	// It blows up during the settle.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		Player *bob   = Join(s, 2, "bob");
		Vehicle *car  = Claim(s, *alice, 91);
		s.NoteExitVehicle(*alice, car->netId);
		s.DestroyVehicle(car->netId);
		Check(s.VehicleHitRecipient(car->netId, bob->id) == nullptr,
		      "the rest of the burst is not relayed onto the wreck");
	}
}

void TestOnlyTheCustodianWritesOffACarInCustody() {
	std::printf("\na car blown up while somebody is settling it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);

	UnownedVehicleKey k{};
	k.kind = UNOWNED_SESSION;
	k.id   = car->netId;

	// Every client but the custodian refuses to blow it up, so a report from
	// one of them is a copy that went its own way.
	Check(!s.NoteUnownedBlowUp(k, bob->id, 0),
	      "bob may not write off the car alice is settling");
	Check(!s.FindVehicle(car->netId)->destroyed, "and it is not written off");

	Check(s.NoteUnownedBlowUp(k, alice->id, 10), "alice may");
	Check(s.FindVehicle(car->netId)->destroyed, "and the session agrees");
	Check(s.CustodianOf(car->netId) == INVALID_PLAYER,
	      "which ends her custody: a wreck has nothing left to settle");
	Check(!s.NoteUnownedBlowUp(k, bob->id, 20) && !s.NoteUnownedBlowUp(k, alice->id, 30),
	      "and it is written off once");
}

// A hit on a session car nobody holds. Every machine pins such a car's health
// at the last report, so a hit taken locally lasted a frame and a parked car
// could not be shot into a fire. The shooter becomes its custodian instead,
// and its engine keeps the health and the fire timer for everybody.
void TestAHitOnACarNobodyHoldsMakesTheShooterItsCustodian() {
	std::printf("\na hit on a session car nobody holds\n");
	using HC = Session::HitCustody;
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);
	s.EndCustody(car->netId, alice->id);
	Check(s.CustodianOf(car->netId) == INVALID_PLAYER, "parked, nobody holds it");

	Check(s.CustodyForHit(car->netId, bob->id) == HC::Granted,
	      "bob shoots it and it is his to settle");
	Check(s.CustodianOf(car->netId) == bob->id && s.MayReportVehicle(bob->id, car->netId),
	      "so his snapshot is the one that carries its health");
	Check(s.CustodyForHit(car->netId, bob->id) == HC::AlreadyTheirs,
	      "his next round, fired before he heard, goes back to him");
	Check(s.CustodyForHit(car->netId, carol->id) == HC::NotTheirs &&
	          s.VehicleHitRecipient(car->netId, carol->id) == bob,
	      "carol's goes to him too, the way any custody's does");

	// Somebody joining now is told who is settling it, after the seats.
	{
		Player *dave = Join(s, 4, "dave");
		const Backfill back = s.BuildBackfill(dave->id, 5);
		bool told = false;
		for (const S_VehicleCustody &c : back.custodies)
			told = told || (c.netId == car->netId && c.playerId == bob->id);
		Check(told && back.custodies.size() == 1,
		      "a joiner hears whose custody it is, and of no other car");
		s.RemovePeer(dave->peer);
	}

	UnownedVehicleKey k{};
	k.kind = UNOWNED_SESSION;
	k.id   = car->netId;
	Check(!s.NoteUnownedBlowUp(k, carol->id, 0),
	      "only he may say it burned out - his is the one fire timer running");
	Check(s.NoteUnownedBlowUp(k, bob->id, 10), "and he may");
	Check(s.CustodyForHit(car->netId, carol->id) == HC::NotTheirs,
	      "a wreck is nobody's to take");

	// A shove is only ever from somebody at the wheel of another car.
	Check(!s.MayPush(carol->id, car->netId), "carol on foot pushes nothing");
	Vehicle *carols = Claim(s, *carol, 93);
	Check(s.MayPush(carol->id, car->netId), "carol at the wheel of another car can");
	Check(!s.MayPush(carol->id, carols->netId), "though not push the car carol is in");
	Check(!s.MayPush(9, car->netId), "and nobody who is not here can");

	// With a driver it is the driver's, whoever shoots it.
	Vehicle *car2 = Claim(s, *alice, 92);
	Check(s.CustodyForHit(car2->netId, bob->id) == HC::NotTheirs &&
	          s.CustodianOf(car2->netId) == INVALID_PLAYER,
	      "a driven car gives nobody custody");
	Check(s.CustodyForHit(9999, bob->id) == HC::NotTheirs, "nor a car there isn't");
	s.NoteExitVehicle(*alice, car2->netId);
	s.EndCustody(car2->netId, alice->id);
	Check(s.CustodyForHit(car2->netId, 7) == HC::NotTheirs &&
	          s.CustodianOf(car2->netId) == INVALID_PLAYER,
	      "nor a player there isn't");
}

// ---- shooting somebody else's traffic (docs/protocol.md §1.23) -------------
//
// The same inversion with the host in the driver's place. The rule it inverts
// is NoteCarState's: only the host streams a traffic car, so only somebody who
// isn't the host gets to report a hit on it, and the host is who hears it.
void TestAHitOnTrafficIsRoutedToItsHost() {
	std::printf("\na hit on a traffic car goes to the machine hosting it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");
	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 5.0f));
	const uint16_t netId = car->netId;

	Check(s.CarHitRecipient(netId, bob->id) == alice,
	      "bob's hit on alice's traffic goes to alice");
	Check(s.CarHitRecipient(netId, carol->id) == alice,
	      "carol's too, and only to her");
	Check(s.CarHitRecipient(netId, alice->id) == nullptr,
	      "alice can't report one on her own traffic - her engine already applied it");
	Check(s.CarHitRecipient(9999, bob->id) == nullptr,
	      "a car the session never named is refused");

	// The pair, stated together because the pair is the invariant.
	AmbientCarState moved{};
	moved.netId = netId;
	moved.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	Check(s.NoteCarState(moved, alice->id) &&
	          s.CarHitRecipient(netId, alice->id) == nullptr,
	      "the host streams the car and is never told about hits on it");
	Check(!s.NoteCarState(moved, bob->id) && s.CarHitRecipient(netId, bob->id) == alice,
	      "everybody else lands hits on it and never streams it");

	// Nothing is recorded, so asking twice gives the same answer.
	Check(s.CarHitRecipient(netId, bob->id) == alice &&
	          s.CarHitRecipient(netId, bob->id) == alice,
	      "and the session keeps no record of a hit");
}

void TestTrafficThatIsGoneTakesNoHits() {
	std::printf("a traffic car that is a wreck, promoted, or hostless takes no hits\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	// Burned out, as its host reported. A burst in flight when it went up is
	// the ordinary case.
	const uint16_t burnt = s.AddCar(alice->id, CarBody(91, 1.0f))->netId;
	Check(s.NoteUnownedBlowUp(AmbientKey(burnt), alice->id, 0), "alice writes it off");
	Check(s.CarHitRecipient(burnt, bob->id) == nullptr, "and hits on the shell stop");

	// Promoted: somebody took the wheel and it's a session car now. A hit
	// that was in flight across that is dropped, not rerouted - the driven
	// car's own exchange takes over from the next shot.
	const uint16_t taken = s.AddCar(alice->id, CarBody(92, 2.0f))->netId;
	uint8_t        wasOwner = INVALID_PLAYER;
	AmbientCarBody body{};
	Check(s.PromoteCar(taken, bob->id, wasOwner, body) != nullptr, "bob takes one");
	Check(s.CarHitRecipient(taken, bob->id) == nullptr &&
	          s.CarHitRecipient(taken, alice->id) == nullptr,
	      "and it's no longer traffic, so this exchange has nobody to tell");

	// The host left. The server hands their cars on or drops them first
	// (Session::HandOverAmbientOf), but the lookup mustn't depend on that.
	const uint16_t orphan = s.AddCar(alice->id, CarBody(93, 3.0f))->netId;
	s.RemovePeer(1);
	Check(s.CarHitRecipient(orphan, bob->id) == nullptr,
	      "a host who has gone can't be told anything");
}

void TestTrafficAndDrivenHitsDoNotCross() {
	std::printf("the two hit exchanges each answer only for their own kind of car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint16_t traffic = s.AddCar(alice->id, CarBody(91, 1.0f))->netId;
	Vehicle *driven = Claim(s, *alice, 90);

	Check(s.VehicleHitRecipient(traffic, bob->id) == nullptr,
	      "C_VehicleHit naming a traffic car goes nowhere");
	Check(s.CarHitRecipient(driven->netId, bob->id) == nullptr,
	      "and C_CarHit naming a driven car goes nowhere");
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

// ---------------------------------------------------------------------------
// The two street-object relays
// ---------------------------------------------------------------------------
//
// The server's whole job for a broken or a knocked-over street object is to
// stamp the sender and pass it on: no row, no table, no backfill. That is a
// decision rather than an omission, and docs/objects.md 1 is the argument -
// CPopulation::ManagePopulation converts the object back to a pristine dummy
// 80 m out and throws the state away, so anything remembered here would
// outlive the fact it recorded.
//
// What there *is* to test without a socket is the one thing that would
// actually break: the agreement between the opcode a client sends and the
// size the server's dispatch is willing to read. Message::as<T> is exactly
// that check and nothing else, so driving it here drives the real gate.
template <class T>
Message Packet(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

void TestAStreetObjectRelayIsStampedAndPassedOn() {
	std::printf("\nstreet objects: what the server is willing to read\n");

	C_ObjectSettled in;
	in.hdr.opcode           = C_ObjectSettled::OPCODE;
	in.body.ident.pos       = {1234.5f, -678.0f, 12.0f};
	in.body.ident.modelIndex = 1393;
	in.body.right           = {1.0f, 0.0f, 0.0f};
	in.body.forward         = {0.0f, 0.0f, -1.0f};
	in.body.up              = {0.0f, 1.0f, 0.0f};
	in.body.pos             = {1235.8f, -678.3f, 10.4f};

	const Message m = Packet(in);
	Check(m.as<C_ObjectSettled>() != nullptr,
	      "a resting place of the right size and opcode is read");
	Check(m.as<C_ObjectBroken>() == nullptr,
	      "and is never mistaken for a break - the two are different opcodes "
	      "and different sizes");

	// A truncated one is refused rather than read short, which is the whole
	// reason the server's dispatch goes through as<T> instead of casting.
	Message cut = m;
	cut.data.pop_back();
	Check(cut.as<C_ObjectSettled>() == nullptr, "a byte short is not a packet");

	// The relay is the client packet with one byte in front of the body, and
	// that byte is the only thing the server adds. Pinning the arithmetic
	// here is what stops a field being quietly added to one half.
	Check(sizeof(S_ObjectSettled) == sizeof(C_ObjectSettled) + 1,
	      "the relay is the report plus the reporter's id and nothing else");
	Check(sizeof(S_ObjectBroken) == sizeof(C_ObjectBroken) + 1,
	      "same as the break, which is the shape this copies");

	// And the reason there is no backfill to test: a joiner is handed
	// vehicles, their damage and the roster, and nothing about the map.
	Session s;
	Player *alice = Join(s, 1, "alice");
	(void)alice;
	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 1);
	Check(back.vehicles.empty() && back.vehicleDamage.empty(),
	      "a joiner in an empty session is handed nothing, and a lamp post "
	      "somebody flattened is not on that list either - the engine will "
	      "have stood it back up by the time he is in");
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

// ---- who simulates a car nobody is driving (protocol.h, S_VehicleCustody) --

void TestGettingOutHandsTheCarToTheDriverWhoLeftIt() {
	std::printf("\nwho settles a car nobody is driving\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);

	Check(s.CustodianOf(car->netId) == INVALID_PLAYER,
	      "a car with a driver has no custodian - the driver is simulating it");

	s.NoteExitVehicle(*alice, car->netId);
	Check(s.CustodianOf(car->netId) == alice->id,
	      "and the moment she gets out it is hers to settle");
	Check(car->driverPlayerId == INVALID_PLAYER,
	      "which is not the same thing as still driving it");

	// The whole of the stability argument in one line: the session host has
	// nothing to do with this. bob is the lowest-numbered player and would be
	// the host in any election, and a car alice parked is not his to simulate
	// - his engine may not even have the street it is standing in.
	Check(s.CustodianOf(car->netId) != bob->id,
	      "and not the host's, whoever the host happens to be");
}

void TestOnlyTheCustodianCanHandACarBack() {
	std::printf("\nending a custody\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);

	Check(!s.EndCustody(car->netId, bob->id),
	      "bob cannot declare somebody else's car settled");
	Check(s.CustodianOf(car->netId) == alice->id, "so it is still alice's");
	Check(!s.EndCustody(999, alice->id), "nor can anybody for a car that is not there");
	Check(s.EndCustody(car->netId, alice->id), "the custodian can");
	Check(s.CustodianOf(car->netId) == INVALID_PLAYER,
	      "and then nobody is simulating it, which is a car everyone pins");
	Check(!s.EndCustody(car->netId, alice->id),
	      "and she cannot end it twice");
}

void TestADriverEndsACustody() {
	std::printf("\nsomebody gets into a car that was settling\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);
	Check(s.CustodianOf(car->netId) == alice->id, "alice is settling it");

	s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
	Check(car->driverPlayerId == bob->id, "bob is driving it now");
	Check(s.CustodianOf(car->netId) == INVALID_PLAYER,
	      "so nobody is settling it - a driver ends a custody with no packet "
	      "spent saying so");
	Check(s.MayReportVehicle(bob->id, car->netId), "and bob reports it");
	Check(!s.MayReportVehicle(alice->id, car->netId),
	      "and alice does not, although she held it a moment ago");
}

void TestAPassengerDoesNotEndACustody() {
	std::printf("\nsomebody gets into the back of a car that was settling\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	s.NoteExitVehicle(*alice, car->netId);

	s.NoteEnterVehicle(*bob, *car, /*seat=*/2);
	Check(s.CustodianOf(car->netId) == alice->id,
	      "a passenger changes nothing about who is simulating it");
	Check(!s.MayReportVehicle(bob->id, car->netId), "and he still may not report it");
}

void TestACustodianLeavingGivesTheCarBackToNobody() {
	std::printf("\nthe machine settling a car disconnects\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteExitVehicle(*alice, netId);
	Check(s.CustodianOf(netId) == alice->id, "alice is settling it");

	s.RemovePeer(1);
	// Handed to nobody rather than to the next player along. The only machine
	// worth giving a driverless car to is one that has it streamed in, and
	// after a disconnect the session does not know that about anybody - so it
	// falls back to the behaviour that was there before custody existed:
	// everybody pins it where it stands.
	Check(s.CustodianOf(netId) == INVALID_PLAYER,
	      "and nobody inherits it when she drops out");
	Check(s.FindVehicle(netId) != nullptr, "the car itself is still there");
}

void TestAWreckIsNotHandedToAnybodyToSettle() {
	std::printf("\na car that blew up has nothing left to settle\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;

	s.DestroyVehicle(netId);
	Check(s.CustodianOf(netId) == INVALID_PLAYER,
	      "blowing up takes the driver out and hands the shell to nobody");
	Check(!s.MayReportVehicle(alice->id, netId),
	      "so nothing more is reported about it");

	// And the other order: a car that was already being settled and then went
	// up underneath its custodian.
	Session t;
	Player *bea = Join(t, 1, "bea");
	Vehicle *c2 = Claim(t, *bea, 91);
	t.NoteExitVehicle(*bea, c2->netId);
	Check(t.CustodianOf(c2->netId) == bea->id, "bea is settling it");
	t.DestroyVehicle(c2->netId);
	Check(t.CustodianOf(c2->netId) == INVALID_PLAYER,
	      "and the blast ends that too");
}

// ---- the dents a settling car takes ----------------------------------------
//
// Server::OnVehicleDamage is these two calls and a relay, in this order. The
// relay goes to everyone but the reporter, so "reaches everyone" is the gate
// saying yes and the merge calling it news.
bool ReportDent(Session &s, uint8_t playerId, const VehicleDamageBody &in,
                VehicleDamageBody &out) {
	return s.MayReportVehicle(playerId, in.netId) && s.NoteVehicleDamage(in, out);
}

void TestTheCustodianReportsTheDentsOfASettle() {
	std::printf("\na dent a car takes while somebody settles it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteExitVehicle(*alice, netId);
	Check(s.CustodianOf(netId) == alice->id, "alice is settling it");

	VehicleDamageBody out{};
	Check(ReportDent(s, alice->id, Dent(netId, 1, 2, 0, 2), out),
	      "her dent is taken, with nobody at the wheel");
	Check(GetPanelLevel(out.panels, 1) == 2 && GetDoorLevel(out.doors, 0) == 2,
	      "and relayed as what the session now holds");
	Check(!ReportDent(s, bob->id, Dent(netId, 3, 3), out),
	      "bob's copy is pinned and collision-proof; his is refused");

	Player *carol = Join(s, 3, "carol");
	const Backfill back = s.BuildBackfill(carol->id, 1);
	Check(back.vehicleDamage.size() == 1 &&
	          GetPanelLevel(back.vehicleDamage[0].body.panels, 1) == 2,
	      "and a joiner gets the car as the settle left it");
}

void TestADentAfterACustodyEndsIsRefused() {
	std::printf("\nthe ways a custody ends, and a dent after each\n");

	// It settled.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		const uint16_t netId = Claim(s, *alice, 91)->netId;
		s.NoteExitVehicle(*alice, netId);
		VehicleDamageBody out{};
		Check(ReportDent(s, alice->id, Dent(netId, 0, 1), out),
		      "the last dent before C_VehicleSettled lands");
		s.EndCustody(netId, alice->id);
		Check(!ReportDent(s, alice->id, Dent(netId, 0, 3), out),
		      "settled: one after it doesn't");
	}

	// Somebody got in.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		Player *bob   = Join(s, 2, "bob");
		Vehicle *car  = Claim(s, *alice, 91);
		const uint16_t netId = car->netId;
		s.NoteExitVehicle(*alice, netId);
		s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
		VehicleDamageBody out{};
		Check(!ReportDent(s, alice->id, Dent(netId, 0, 3), out),
		      "a new driver: the old custodian is refused");
		Check(ReportDent(s, bob->id, Dent(netId, 0, 3), out), "and he is taken");
	}

	// The custodian quit.
	{
		Session s;
		Player *alice = Join(s, 1, "alice");
		Player *bob   = Join(s, 2, "bob");
		const uint16_t netId = Claim(s, *alice, 91)->netId;
		s.NoteExitVehicle(*alice, netId);
		s.RemovePeer(1);
		VehicleDamageBody out{};
		Check(!ReportDent(s, bob->id, Dent(netId, 0, 3), out),
		      "custodian gone: nobody inherits the right to dent it");
	}
}

void TestALaterDriversLighterDentIsStillNews() {
	std::printf("\na lighter dent after a settle that recorded a heavy one\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteExitVehicle(*alice, netId);

	VehicleDamageBody out{};
	ReportDent(s, alice->id, Dent(netId, 0, 3), out);
	s.EndCustody(netId, alice->id);

	s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
	Check(ReportDent(s, bob->id, Dent(netId, 4, 1), out),
	      "bob's scrape on another panel is news");
	Check(!ReportDent(s, bob->id, Dent(netId, 0, 1), out),
	      "on the wing the settle already took off, it isn't - the car is worse");

	VehicleDamageBody repair{};
	repair.netId  = netId;
	repair.panels = VEH_DAMAGE_RESET;
	Check(ReportDent(s, bob->id, repair, out), "he takes it through a spray shop");
	Check(ReportDent(s, bob->id, Dent(netId, 0, 1), out),
	      "and now the same lighter scrape is news again");
	Check(GetPanelLevel(out.panels, 0) == 1 && GetPanelLevel(out.panels, 4) == 0,
	      "the record is the car after the spray, not the settle's dents");
}

// ---- a traffic car's dents, from its host ------------------------------------

void TestATrafficCarsDentsComeFromItsHost() {
	std::printf("\na traffic car's dents\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint16_t netId = s.AddCar(alice->id, CarBody(91, 5.0f))->netId;

	VehicleDamageBody out{};
	Check(!s.MayReportVehicle(alice->id, netId),
	      "the session-car gate says no, as it always did for traffic");
	Check(s.NoteCarDamage(alice->id, Dent(netId, 1, 2), out),
	      "its host's engine is the one denting it, so its host may say so");
	Check(out.netId == netId && GetPanelLevel(out.panels, 1) == 2,
	      "and what goes out is the session's record");
	Check(!s.NoteCarDamage(bob->id, Dent(netId, 3, 3), out),
	      "a replica's dents are its own machine's business");
	Check(GetPanelLevel(s.FindCar(netId)->damagePanels, 3) == 0, "and are not recorded");
	Check(!s.NoteCarDamage(alice->id, Dent(netId, 1, 1), out),
	      "a milder report adds nothing and is not relayed");
	Check(s.NoteCarDamage(alice->id, Dent(netId, 1, 1, 2, 2), out) &&
	          GetPanelLevel(out.panels, 1) == 2 && GetDoorLevel(out.doors, 2) == 2,
	      "a door on top keeps the worse panel");

	VehicleDamageBody reset{};
	reset.netId  = netId;
	reset.panels = VEH_DAMAGE_RESET;
	Check(!s.NoteCarDamage(alice->id, reset, out) &&
	          GetPanelLevel(s.FindCar(netId)->damagePanels, 1) == 2,
	      "traffic is never resprayed, so a repair marker for it is refused");

	Check(!s.NoteCarDamage(alice->id, Dent(999, 1, 2), out), "a car nobody named is nothing");

	Vehicle *mine = Claim(s, *bob, 105);
	Check(!s.NoteCarDamage(bob->id, Dent(mine->netId, 1, 2), out),
	      "and a session car still goes through its own gate, not this one");

	UnownedVehicleKey key{};
	key.kind = UNOWNED_AMBIENT;
	key.id   = netId;
	s.NoteUnownedBlowUp(key, alice->id, 10);
	Check(!s.NoteCarDamage(alice->id, Dent(netId, 4, 3), out), "a wreck is finished");
}

void TestAJoinerAndANewDriverGetTheDents() {
	std::printf("\na traffic car's dents reach a joiner and a new driver\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint16_t dented = s.AddCar(alice->id, CarBody(91, 5.0f))->netId;
	s.AddCar(alice->id, CarBody(105, 6.0f));
	VehicleDamageBody out{};
	s.NoteCarDamage(alice->id, Dent(dented, 0, 3, 1, 2), out);

	const Backfill back = s.BuildBackfill(bob->id, 1);
	Check(back.cars.size() == 2, "bob is told about both cars");
	size_t forDented = 0;
	for (const S_VehicleDamage &d : back.vehicleDamage)
		if (d.body.netId == dented && GetPanelLevel(d.body.panels, 0) == 3 &&
		    GetDoorLevel(d.body.doors, 1) == 2 && d.playerId == INVALID_PLAYER)
			++forDented;
	Check(forDented == 1 && back.vehicleDamage.size() == 1,
	      "and about the dents of the one that has any, as the session's record");

	uint8_t        wasOwner = INVALID_PLAYER;
	AmbientCarBody body{};
	Vehicle *promoted = s.PromoteCar(dented, bob->id, wasOwner, body);
	Check(promoted != nullptr && GetPanelLevel(promoted->damagePanels, 0) == 3 &&
	          GetDoorLevel(promoted->damageDoors, 1) == 2,
	      "and somebody taking the wheel keeps them on the session car");
}

// ---- a traffic car that has stopped being traffic (S_CarPromoted) ----------

void TestGettingIntoSomebodyElsesTrafficMovesOwnership() {
	std::printf("\na player takes the wheel of somebody else's traffic\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 5.0f));
	const uint16_t netId = car->netId;

	uint8_t        wasOwner = INVALID_PLAYER;
	AmbientCarBody body{};
	Vehicle *promoted = s.PromoteCar(netId, bob->id, wasOwner, body);
	Check(promoted != nullptr, "the session promotes it");
	// The netId is the whole point. Every machine already has this CVehicle
	// filed under this number, and keeping it is what lets them move the
	// bookkeeping instead of destroying and rebuilding the car.
	Check(promoted != nullptr && promoted->netId == netId,
	      "under the same netId, so nothing anywhere has to be rebuilt");
	Check(wasOwner == alice->id, "and it says whose traffic it was");
	Check(promoted != nullptr && promoted->modelId == 91,
	      "carrying the identity the ambient row was holding");

	Check(s.FindCar(netId) == nullptr,
	      "the traffic row is gone, so its old host is not asked about it again");
	AmbientCarState stale{};
	stale.netId = netId;
	stale.pos   = {9.0f, 9.0f, 9.0f};
	stale.rot   = {0.0f, 0.0f, 0.0f, 1.0f};
	Check(!s.NoteCarState(stale, alice->id),
	      "and a state from that host lands nowhere");

	s.NoteEnterVehicle(*bob, *promoted, /*seat=*/0);
	Check(s.MayReportVehicle(bob->id, netId), "the new driver reports it");
	Check(!s.MayReportVehicle(alice->id, netId), "and its old host does not");
}

void TestAPromotionNeedsATrafficCarThatIsActuallyThere() {
	std::printf("\nwhat cannot be promoted\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	uint8_t        wasOwner = INVALID_PLAYER;
	AmbientCarBody body{};
	Check(s.PromoteCar(999, bob->id, wasOwner, body) == nullptr,
	      "a number the session has never heard of promotes nothing");
	Check(wasOwner == INVALID_PLAYER, "and names nobody");

	// A session car is already a session car. The netId space is one space
	// (AllocNetId), so this cannot happen - the check is the statement that
	// it cannot, not a case anybody reaches.
	Vehicle *mine = Claim(s, *alice, 91);
	Check(s.PromoteCar(mine->netId, bob->id, wasOwner, body) == nullptr,
	      "and a car that is already a session car is not promoted twice");

	// A burnt shell is not a car anybody is driving away.
	AmbientCar *car = s.AddCar(alice->id, CarBody(91, 5.0f));
	const uint16_t netId = car->netId;
	s.NoteUnownedBlowUp(AmbientKey(netId), alice->id, 0);
	Check(s.PromoteCar(netId, bob->id, wasOwner, body) == nullptr,
	      "and neither is a traffic car its host has already written off");
}

// ---------------------------------------------------------------------------
// Rampages - docs/roadmap.md 5.10
// ---------------------------------------------------------------------------
//
// What the server holds is small and every one of these is about a race the
// live game would hand over once, at speed, with no way to watch it.

RampageStartBody Start(int32_t limitMs, uint16_t target) {
	RampageStartBody b{};
	b.limitMs = limitMs;
	b.target  = target;
	return b;
}

RampageKillBody Kill(uint16_t frenzyId, uint16_t model) {
	RampageKillBody b{};
	b.frenzyId = frenzyId;
	b.model    = model;
	b.weapon   = 6;   // M16, the weapon rampage 01 is started with
	b.flags    = 0;
	return b;
}

RampageEndBody End(uint16_t frenzyId, uint8_t outcome) {
	RampageEndBody b{};
	b.frenzyId = frenzyId;
	b.outcome  = outcome;
	return b;
}

void TestOneFrenzyAtATimeAndEverybodyIsPutInIt() {
	std::printf("\none rampage, and every machine's own script joins it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	// Both machines' rampage.sc start the same frenzy off their own
	// aPickUpsCollected, a round trip apart.
	RampageOpenBody first{}, second{};
	Check(s.NoteRampageStart(alice->id, Start(120000, 20), 1000, first),
	      "alice's script opens the session's frenzy");
	Check(first.frenzyId != 0, "and it is named");
	Check(first.killsNeeded == 20, "playing for the number the script asked for");
	Check(first.elapsedMs == 0, "and it has just started");

	Check(s.NoteRampageStart(bob->id, Start(120000, 20), 1080, second),
	      "bob's script is answered too");
	Check(second.frenzyId == first.frenzyId,
	      "with the frenzy that is already open, not a second one");
	Check(second.elapsedMs == 80, "and how far into it he is");
	Check(s.CurrentRampage().openedBy == alice->id,
	      "the session remembers whose report was first");
}

void TestAJoinerIsPutIntoTheRampageAlreadyRunning() {
	std::printf("\njoining a rampage that is already half over\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 0, open);
	for (int i = 0; i < 12; ++i)
		Check(s.NoteRampageKill(Kill(open.frenzyId, 108)), "a kill lands");

	// He is backfilled with the KILLFRENZY collection like any other pickup,
	// so his own script starts a fresh 120-second, 20-kill frenzy and says
	// so. Without an answer he would sit in his script's wait loop forever.
	Player         *carl = Join(s, 2, "carl");
	RampageOpenBody late{};
	Check(s.NoteRampageStart(carl->id, Start(120000, 20), 65000, late),
	      "the joiner's own start is answered");
	Check(late.frenzyId == open.frenzyId, "with the session's frenzy");
	Check(late.killsNeeded == 8, "and the eight kills that are actually left");
	Check(late.elapsedMs == 65000, "and the 65 seconds already gone");
}

void TestAKillForAnOldFrenzyIsNotCountedAgainstTheNewOne() {
	std::printf("\na kill that arrives after its own rampage ended\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody one{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 0, one);
	RampageEndBody verdict{};
	Check(s.NoteRampageEnd(End(one.frenzyId, RAMPAGE_FAILED), verdict),
	      "it fails");

	// rampage.sc destroys the pickup and creates a new one at the second
	// position within a frame or two, so the next frenzy really can be
	// seconds away.
	RampageOpenBody two{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 4000, two);
	Check(two.frenzyId != one.frenzyId, "the next one gets its own name");

	Check(!s.NoteRampageKill(Kill(one.frenzyId, 108)),
	      "a straggler for the old frenzy is dropped");
	Check(s.CurrentRampage().kills == 0, "and does not count against the new one");
	Check(s.NoteRampageKill(Kill(two.frenzyId, 108)), "the new one's kills do");
}

void TestTheFirstEndingWins() {
	std::printf("\ntwo machines disagreeing about how a rampage ended\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 0, open);

	// The race this exists for: bob's clock runs out while alice's winning
	// kill is still on the wire. Whichever report the server sees first is
	// the session's, and the other is dropped - so nobody is told the
	// rampage failed after being told it passed.
	RampageEndBody first{}, second{};
	Check(s.NoteRampageEnd(End(open.frenzyId, RAMPAGE_FAILED), first),
	      "bob's timeout gets there first");
	Check(first.outcome == RAMPAGE_FAILED, "and that is the verdict");
	Check(!s.NoteRampageEnd(End(open.frenzyId, RAMPAGE_PASSED), second),
	      "alice's pass is dropped");
	(void)bob;
}

void TestTheServerEndsARampageNobodyIsLeftToEnd() {
	std::printf("\na rampage whose owner disconnected\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 5000, open);

	RampageEndBody ended{};
	Check(!s.ExpireRampage(5000 + 120000, ended),
	      "the deadline itself is not enough - the grace has to pass too");
	Check(s.ExpireRampage(5000 + 120000 + Session::RAMPAGE_GRACE_MS, ended),
	      "a second later the session calls it");
	Check(ended.outcome == RAMPAGE_FAILED && ended.frenzyId == open.frenzyId,
	      "as a failure, for the frenzy that was open");
	Check(!s.ExpireRampage(5000 + 300000, ended), "and only once");
}

void TestARampageWithNoTimeLimitNeverTimesOut() {
	std::printf("\na rampage the script gave no time limit\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(-1, 20), 0, open);

	// CDarkel::Update takes the ongoing arm whenever TimeLimit is negative
	// (`cmp dword [00885BACh],0 / jl` at 0x00420696), so a server that
	// timed this one out would end a rampage the engine never would.
	RampageEndBody ended{};
	Check(!s.ExpireRampage(10u * 60u * 1000u, ended), "ten minutes in, still open");
	Check(s.CurrentRampage().open, "because the engine would not end it either");
}

void TestTheTargetIsScaledByThePlayerCount() {
	std::printf("\nthe scaled rule\n");
	// The arithmetic on its own first, because it is shared with the client
	// and has the awkward cases.
	Check(ScaledRampageTarget(20, RAMPAGE_RULE_SHARED, 4) == 20,
	      "the default rule leaves the script's number alone");
	Check(ScaledRampageTarget(20, RAMPAGE_RULE_SCALED, 1) == 20,
	      "one player has nobody to share with");
	Check(ScaledRampageTarget(20, RAMPAGE_RULE_SCALED, 0) == 20,
	      "and neither does an empty session");
	Check(ScaledRampageTarget(20, RAMPAGE_RULE_SCALED, 4) == 80, "four players, 80");
	Check(ScaledRampageTarget(500, RAMPAGE_RULE_SCALED, 8) == RAMPAGE_MAX_KILLS,
	      "and it is clamped rather than left to overflow a uint16");

	Session s;
	s.SetRampageRule(RAMPAGE_RULE_SCALED);
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(120000, 20), 0, open);
	Check(open.killsNeeded == 40, "two players murder 40 Diablos, not 20");
	Check(s.CurrentRampage().target == 40, "and the session plays for that");
	(void)bob;
}

void TestWithRampagesOffThereIsNoSessionFrenzy() {
	std::printf("\nrampages off\n");
	Session s;
	s.SetRampageRule(RAMPAGE_RULE_OFF);
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody open{};
	Check(!s.NoteRampageStart(alice->id, Start(120000, 20), 0, open),
	      "no frenzy is opened");
	Check(!s.CurrentRampage().open, "and nothing is held");
	// Which is exactly the behaviour this build had before the feature
	// existed: every machine's own CDarkel, counting its own player.
	Check(!s.NoteRampageKill(Kill(1, 108)), "so a kill has nothing to land on");
	RampageCarBody car{};
	car.frenzyId = 1;
	car.model    = 90;
	car.key.kind = RAMPAGE_CAR_UNKEYED;
	Check(!s.NoteRampageCar(car), "and neither has a car");
}

// ---- vehicle rampages ------------------------------------------------------

RampageCarBody Car(uint16_t frenzyId, uint16_t model, uint8_t kind, uint16_t id) {
	RampageCarBody b{};
	b.frenzyId = frenzyId;
	b.model    = model;
	b.key.kind = kind;
	b.key.pad  = 0;
	b.key.id   = id;
	return b;
}

void TestANamedCarCountsOnceHoweverManyMachinesSawIt() {
	std::printf("\nrampage 02: a parked car two machines blew up\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Join(s, 2, "bob");

	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(120000, 13), 0, open);

	// Each machine's copy of a parked car went up in its own copy of the same
	// rocket, and each engine reported it.
	Check(s.NoteRampageCar(Car(open.frenzyId, 95, UNOWNED_PARKED, 41)),
	      "the first report of a parked car is counted and relayed");
	Check(!s.NoteRampageCar(Car(open.frenzyId, 95, UNOWNED_PARKED, 41)),
	      "the second machine's report of the same car is dropped");
	Check(s.CurrentRampage().kills == 1, "one car");

	Check(s.NoteRampageCar(Car(open.frenzyId, 116, UNOWNED_SESSION, 41)),
	      "the same id under another kind is another car");
	Check(!s.NoteRampageCar(Car(open.frenzyId, 116, UNOWNED_SESSION, 41)),
	      "and it counts once too");

	// Traffic, somebody's own car, a car only one machine has. One machine
	// can decide each of those, so two reports are two cars.
	Check(s.NoteRampageCar(Car(open.frenzyId, 90, RAMPAGE_CAR_UNKEYED, 0)),
	      "an unkeyed car is counted");
	Check(s.NoteRampageCar(Car(open.frenzyId, 90, RAMPAGE_CAR_UNKEYED, 0)),
	      "and so is the next one, same model and all");
	Check(s.NoteRampageCar(Car(open.frenzyId, 90, UNOWNED_AMBIENT, 12)),
	      "traffic isn't deduplicated: only its host reports it");
	Check(s.NoteRampageCar(Car(open.frenzyId, 90, UNOWNED_AMBIENT, 12)),
	      "so a second report would be a second car, which is the host's call");
	Check(s.CurrentRampage().kills == 6, "six cars in all");

	// A joiner is told what's left, cars and kills in the same number.
	RampageOpenBody late{};
	Player         *carl = Join(s, 3, "carl");
	s.NoteRampageStart(carl->id, Start(120000, 13), 30000, late);
	Check(late.killsNeeded == 7, "a joiner is told seven cars are left");
}

void TestACarForAnOldFrenzyIsDropped() {
	std::printf("\na car that arrives after its own rampage ended\n");
	Session s;
	Player *alice = Join(s, 1, "alice");

	RampageOpenBody one{};
	s.NoteRampageStart(alice->id, Start(120000, 13), 0, one);
	Check(s.NoteRampageCar(Car(one.frenzyId, 95, UNOWNED_PARKED, 41)), "counted");
	RampageEndBody verdict{};
	s.NoteRampageEnd(End(one.frenzyId, RAMPAGE_FAILED), verdict);

	RampageOpenBody two{};
	s.NoteRampageStart(alice->id, Start(120000, 13), 4000, two);
	Check(!s.NoteRampageCar(Car(one.frenzyId, 95, UNOWNED_PARKED, 42)),
	      "a straggler for the old frenzy is dropped");
	Check(s.CurrentRampage().kills == 0, "and doesn't count against the new one");
	Check(s.NoteRampageCar(Car(two.frenzyId, 95, UNOWNED_PARKED, 41)),
	      "a name counted in the last frenzy counts again in this one");
}

void TestTheOldestCarNameGoesFirst() {
	std::printf("\nmore named cars than the frenzy remembers\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	RampageOpenBody open{};
	s.NoteRampageStart(alice->id, Start(-1, 1000), 0, open);

	for (uint16_t i = 0; i <= Session::RAMPAGE_CAR_KEYS; ++i)
		s.NoteRampageCar(Car(open.frenzyId, 95, UNOWNED_PARKED, i));
	Check(s.CurrentRampage().kills == Session::RAMPAGE_CAR_KEYS + 1,
	      "every one of them counted");
	Check(!s.NoteRampageCar(Car(open.frenzyId, 95, UNOWNED_PARKED,
	                            Session::RAMPAGE_CAR_KEYS)),
	      "the newest name is still remembered");
	Check(s.NoteRampageCar(Car(open.frenzyId, 95, UNOWNED_PARKED, 0)),
	      "the oldest one was let go to make room");
}

// ---- police helicopters ------------------------------------------------------

HeliStateBody Heli(uint16_t serial, uint8_t slot = 0,
                   uint8_t status = HELI_STATUS_CHASE) {
	HeliStateBody b{};
	b.serial = serial;
	b.slot   = slot;
	b.status = status;
	b.rot    = {0.0f, 0.0f, 0.0f, 1.0f};
	return b;
}

HeliGoneBody Gone(uint16_t serial, uint8_t slot = 0,
                  uint8_t reason = HELI_GONE_SHOT_DOWN) {
	HeliGoneBody b{};
	b.serial         = serial;
	b.slot           = slot;
	b.reason         = reason;
	b.creditPlayerId = INVALID_PLAYER;
	return b;
}

HeliHitBody HeliHit(uint8_t owner, uint16_t serial, uint8_t slot = 0) {
	HeliHitBody b{};
	b.ownerPlayerId = owner;
	b.slot          = slot;
	b.serial        = serial;
	b.kind          = HELI_HIT_BULLET;
	b.damage        = 4;
	return b;
}

void TestAHelicopterStateIsRelayedUntilItIsGone() {
	std::printf("\na helicopter's state\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Check(s.NoteHeliState(alice->id, Heli(1)), "a police slot is relayed");
	Check(s.HeliLive(alice->id, 0, 1), "and the session knows it is live");
	Check(!s.NoteHeliState(alice->id, Heli(2, 2)),
	      "the script helicopter's slot is not");
	Check(!s.NoteHeliState(alice->id, Heli(3, 0, 9)), "nor a status the engine lacks");
	Check(s.NoteHeliGone(alice->id, Gone(1)), "its end is relayed");
	Check(!s.HeliLive(alice->id, 0, 1), "and it is no longer live");
	Check(!s.NoteHeliState(alice->id, Heli(1)),
	      "a state that overtook its own end is not relayed");
	Check(s.NoteHeliState(alice->id, Heli(2)), "the next helicopter in the slot is");
}

void TestAHelicopterIsGoneOnce() {
	std::printf("\na helicopter ends once\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	s.NoteHeliState(alice->id, Heli(4, 1));
	Check(s.NoteHeliGone(alice->id, Gone(4, 1)), "the first report goes out");
	Check(!s.NoteHeliGone(alice->id, Gone(4, 1)), "a second does not");
	Check(!s.NoteHeliGone(alice->id, Gone(5, 3)), "nor one for a slot that isn't police");
	HeliGoneBody odd = Gone(6);
	odd.reason = 9;
	Check(!s.NoteHeliGone(alice->id, odd), "nor one with no reason we know");

	// More ends than the ring holds, and the oldest is let go.
	for (uint16_t i = 100; i < 100 + 9; ++i)
		s.NoteHeliGone(alice->id, Gone(i));
	Check(s.NoteHeliState(alice->id, Heli(100)),
	      "the oldest remembered end is forgotten once the ring wraps");
	Check(!s.NoteHeliState(alice->id, Heli(108)), "the newest is still remembered");
}

void TestAHitGoesToTheOwnerOfALiveHelicopter() {
	std::printf("\na hit on a helicopter\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	s.NoteHeliState(alice->id, Heli(1));

	Check(s.HeliHitRecipient(HeliHit(alice->id, 1), bob->id) == alice,
	      "bob's hit goes to alice");
	Check(s.HeliHitRecipient(HeliHit(alice->id, 1), alice->id) == nullptr,
	      "not to alice from alice; her engine took it already");
	Check(s.HeliHitRecipient(HeliHit(alice->id, 2), bob->id) == nullptr,
	      "not for a serial that isn't live");
	Check(s.HeliHitRecipient(HeliHit(alice->id, 1, 1), bob->id) == nullptr,
	      "not for the wrong slot");
	HeliHitBody huge = HeliHit(alice->id, 1);
	huge.damage = 800;
	Check(s.HeliHitRecipient(huge, bob->id) == nullptr,
	      "not one that would bring it down in a packet");
	s.NoteHeliGone(alice->id, Gone(1));
	Check(s.HeliHitRecipient(HeliHit(alice->id, 1), bob->id) == nullptr,
	      "not after it is gone");
}

HeliShotBody HeliShot(uint16_t serial, uint8_t slot = 0) {
	HeliShotBody b{};
	b.serial = serial;
	b.slot   = slot;
	b.source = {100.0f, 200.0f, 60.0f};
	b.target = {110.0f, 205.0f, 12.0f};
	return b;
}

void TestAHelicopterRoundIsRelayedOnlyFromALiveOne() {
	std::printf("\na round a helicopter fired\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	Check(!s.MayRelayHeliShot(alice->id, HeliShot(1)),
	      "not before the helicopter's first state; nobody has a replica yet");
	s.NoteHeliState(alice->id, Heli(1));
	Check(s.MayRelayHeliShot(alice->id, HeliShot(1)), "relayed while it is live");
	Check(!s.MayRelayHeliShot(bob->id, HeliShot(1)),
	      "not from somebody who isn't its owner");
	Check(!s.MayRelayHeliShot(alice->id, HeliShot(2)), "not for a serial that isn't live");
	Check(!s.MayRelayHeliShot(alice->id, HeliShot(1, 1)), "not for the wrong slot");

	HeliShotBody script = HeliShot(1, 2);
	Check(!s.MayRelayHeliShot(alice->id, script), "not from the script's slot");

	HeliShotBody nan = HeliShot(1);
	nan.target.z = std::numeric_limits<float>::quiet_NaN();
	Check(!s.MayRelayHeliShot(alice->id, nan), "not with a NaN in it");

	HeliShotBody far = HeliShot(1);
	far.target.x = far.source.x + HELI_SHOT_MAX_LENGTH + 1.0f;
	Check(!s.MayRelayHeliShot(alice->id, far), "not a line longer than the gun reaches");
	HeliShotBody reach = HeliShot(1);
	reach.target = {reach.source.x + HELI_SHOT_MAX_LENGTH - 1.0f, reach.source.y,
	                reach.source.z};
	Check(s.MayRelayHeliShot(alice->id, reach), "and one just inside it is");

	s.NoteHeliGone(alice->id, Gone(1));
	Check(!s.MayRelayHeliShot(alice->id, HeliShot(1)),
	      "not once it is gone; a round that overtook the end has nowhere to go");
}

void TestOnlySomebodyElseInTheSessionIsCredited() {
	std::printf("\nwho can be credited with a helicopter\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Check(s.MayCreditHeli(alice->id, bob->id), "bob, for alice's helicopter");
	Check(!s.MayCreditHeli(alice->id, alice->id), "not alice for her own");
	Check(!s.MayCreditHeli(alice->id, 6), "not a slot nobody is in");
	s.RemovePeer(2);
	Check(!s.MayCreditHeli(alice->id, 1), "not bob once he has left");
}

void TestALeavingOwnerTakesHisHelicoptersWithHim() {
	std::printf("\nan owner who leaves\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	const uint8_t aliceId = alice->id;
	s.NoteHeliState(aliceId, Heli(1));
	s.NoteHeliGone(aliceId, Gone(2, 1));
	s.RemovePeer(1);
	Check(!s.HeliLive(aliceId, 0, 1), "his helicopter is not live");
	Check(s.HeliHitRecipient(HeliHit(aliceId, 1), bob->id) == nullptr,
	      "and a hit on it goes nowhere");
	Player *carol = Join(s, 3, "carol");
	Check(carol->id == aliceId, "the slot is handed on");
	Check(s.NoteHeliState(carol->id, Heli(2, 1)),
	      "and her serial 2 is not mistaken for his finished one");
}

} // namespace

// ---- cheats (docs/cheats.md) -----------------------------------------------

CheatBody Cheat(uint8_t id, uint8_t state) {
	CheatBody b{};
	b.cheat = id;
	b.state = state;
	return b;
}

// Every cheat through the server's decision, under every rule, from the host
// and from somebody else. What comes out has to be exactly the route the
// client planned for, or a cheat goes somewhere its typist's machine did not
// expect - a sky to everybody, a riot to nobody.
void TestEveryCheatIsRelayedWhereItsRouteSays() {
	std::printf("\nthe cheat relay, every cheat, every rule\n");
	int wrong = 0;
	for (uint8_t rule = 0; rule <= CHEAT_RULE_OFF; ++rule) {
		for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
			for (int fromHost = 0; fromHost < 2; ++fromHost) {
				// A state the cheat can actually carry, where it has one.
				uint8_t state = 0;
				if (id == CHEAT_MAYHEM || id == CHEAT_WEAPONS_FOR_ALL ||
				    id == CHEAT_FAST_WEATHER)
					state = 1;
				if (id == CHEAT_FAST_TIME || id == CHEAT_SLOW_TIME)
					state = 3;
				const uint8_t got =
				    CheatRelayFor(rule, id, state, fromHost != 0, true);

				uint8_t want = CHEAT_RELAY_DROP;
				if (CheatAllowed(rule, id)) {
					if (CheatRouteOf(id) == CHEAT_ROUTE_EVERYONE)
						want = CHEAT_RELAY_OTHERS;
					else if (CheatRouteOf(id) == CHEAT_ROUTE_HOST && !fromHost)
						want = CHEAT_RELAY_HOST;
				}
				if (got != want) {
					++wrong;
					std::printf("    rule %u cheat %u fromHost %d: got %u want %u\n",
					            rule, id, fromHost, got, want);
				}
			}
		}
	}
	Check(wrong == 0, "138 combinations, each where its route says");

	Check(CheatRelayFor(CHEAT_RULE_SHARED, CHEAT_FOGGY, 0, false, false) ==
	          CHEAT_RELAY_DROP,
	      "a sky with no host to take it goes nowhere");
	Check(CheatRelayFor(CHEAT_RULE_SHARED, CHEAT_MAYHEM, 0, false, true) ==
	          CHEAT_RELAY_DROP,
	      "a riot that claims to be undone is dropped - there is no undo");
	Check(CheatRelayFor(CHEAT_RULE_SHARED, CHEAT_FAST_TIME, 5, false, true) ==
	          CHEAT_RELAY_DROP,
	      "a time scale past 4.0 is dropped");
	Check(CheatRelayFor(CHEAT_RULE_SHARED, CHEAT_WEAPONS, 0, false, true) ==
	          CHEAT_RELAY_DROP,
	      "a personal cheat that turns up anyway is dropped");
	Check(CheatRelayFor(CHEAT_RULE_SHARED, CHEAT_COUNT, 1, false, true) ==
	          CHEAT_RELAY_DROP,
	      "and so is a cheat that does not exist");
}

void TestTheSessionRemembersWhatEverybodyRuns() {
	std::printf("\nthe cheats a joiner has to be brought to\n");
	Session s;
	Player *alice = Join(s, 1, "alice");   // the host
	Player *bob   = Join(s, 2, "bob");

	Check(s.NoteCheat(bob->id, Cheat(CHEAT_FOGGY, 0)) == CHEAT_RELAY_HOST,
	      "bob's fog goes to alice");
	Check(s.NoteCheat(alice->id, Cheat(CHEAT_FOGGY, 0)) == CHEAT_RELAY_DROP,
	      "alice's own fog goes nowhere - her world packet carries it");
	Check(s.NoteCheat(bob->id, Cheat(CHEAT_MAYHEM, 1)) == CHEAT_RELAY_OTHERS,
	      "bob's riot goes to everybody else");
	Check(s.NoteCheat(bob->id, Cheat(CHEAT_WEAPONS_FOR_ALL, 1)) == CHEAT_RELAY_OTHERS,
	      "and so does arming the crowd");
	s.NoteCheat(alice->id, Cheat(CHEAT_WEAPONS_FOR_ALL, 0));   // and alice undoes it
	s.NoteCheat(alice->id, Cheat(CHEAT_FAST_TIME, 3));
	s.NoteCheat(bob->id, Cheat(CHEAT_SLOW_TIME, 1));

	Player *carl = Join(s, 3, "carl");
	const Backfill back = s.BuildBackfill(carl->id, 99);

	bool mayhem = false, weapons = false, time = false, sky = false;
	int  timeRows = 0;
	for (const S_Cheat &c : back.cheats) {
		if (c.playerId != INVALID_PLAYER)
			Check(false, "a replay names nobody");
		if (c.body.cheat == CHEAT_MAYHEM)
			mayhem = c.body.state == 1;
		if (c.body.cheat == CHEAT_WEAPONS_FOR_ALL)
			weapons = c.body.state == 0;
		if (c.body.cheat == CHEAT_FAST_TIME || c.body.cheat == CHEAT_SLOW_TIME) {
			++timeRows;
			time = c.body.cheat == CHEAT_SLOW_TIME && c.body.state == 1;
		}
		if (c.body.cheat == CHEAT_FOGGY)
			sky = true;
	}
	Check(mayhem, "carl is handed the riot");
	Check(weapons, "and the crowd as alice left it, unarmed");
	Check(timeRows == 1 && time,
	      "and one time scale, the last one typed (BOOOOORING's 0.5)");
	Check(!sky, "but no sky - the host's world packet is what carries that");
	Check(back.cheats.size() == 3, "three rows and nothing else");

	// A rule changed to personal since: the riot is no longer the session's.
	s.SetCheatRule(CHEAT_RULE_PERSONAL);
	Check(s.BuildBackfill(carl->id, 100).cheats.empty(),
	      "under personal nobody is brought into a riot");
	Check(s.NoteCheat(bob->id, Cheat(CHEAT_MAYHEM, 1)) == CHEAT_RELAY_DROP,
	      "and a new one is not passed on");
	s.SetCheatRule(CHEAT_RULE_SHARED);

	// Everybody leaves; the next session starts from single player.
	s.RemovePeer(1);
	s.RemovePeer(2);
	s.RemovePeer(3);
	Player *dave = Join(s, 4, "dave");
	Check(s.BuildBackfill(dave->id, 101).cheats.empty(),
	      "an emptied session forgets what its cheats did");
}

void TestTheCheatRuleIsClamped() {
	std::printf("\nthe cheat rule\n");
	Session s;
	Check(s.CheatRuleValue() == CHEAT_RULE_SHARED, "shared by default (5.14)");
	s.SetCheatRule(CHEAT_RULE_OFF);
	Check(s.CheatRuleValue() == CHEAT_RULE_OFF, "off sticks");
	s.SetCheatRule(3);
	Check(s.CheatRuleValue() == CHEAT_RULE_SHARED, "3 is not a rule and means shared");
}

// ---- when a session car stops being one ------------------------------------
//
// VEHICLE_KEEP_RADIUS_M and VEHICLE_RELEASE_MS in session.h. Before these the
// session never let go of a car: every claim took a row for good, the 65th
// claim of a session was refused, and every machine kept a mission car for
// every car anybody had ever taken.

// Somewhere to stand, `metres` east of where Claim parks a car (10, 10).
PlayerStateBody StandAt(float metres) {
	PlayerStateBody b = State(10.0f + metres, 100.0f);
	b.pos.y           = 10.0f;
	return b;
}

void TestALeaversCarGoesToWhoeverIsNextToIt() {
	std::printf("\na leaver's car goes to whoever is next to it\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");
	s.NotePlayerState(*bob, StandAt(30.0f));
	s.NotePlayerState(*carol, StandAt(10.0f));
	const uint16_t netId = Claim(s, *alice, 91)->netId;

	const std::vector<uint16_t> handed = s.HandOverVehiclesOf(alice->id);
	Check(handed.size() == 1 && handed[0] == netId, "the car she was driving is handed on");
	Check(s.CustodianOf(netId) == carol->id, "to the nearer of the two");
	Check(s.FindVehicle(netId)->driverPlayerId == INVALID_PLAYER,
	      "as a settle, never a driver and a custodian at once");
	s.RemovePeer(1);
	Check(s.CustodianOf(netId) == carol->id, "and her leaving does not take it back");
	Check(s.MayReportVehicle(carol->id, netId), "so carol's reports of it are taken");
	Check(!s.MayReportVehicle(bob->id, netId), "and bob's are not");

	// Carol, settling it, leaves too: bob is still near enough.
	Check(s.HandOverVehiclesOf(carol->id).size() == 1 && s.CustodianOf(netId) == bob->id,
	      "a custody is handed on the same way a seat is");
}

void TestTheHistoryIsWhereTheOwnerSaidItWas() {
	std::printf("\nthe history is where the owner said it was\n");
	PoseHistory h;
	Vec3        at{};
	Check(!h.At(1000, at), "nothing said, nothing to compare with");
	h.Note(1000, Vec3{0.0f, 0.0f, 0.0f}, 1);
	h.Note(1040, Vec3{4.0f, 0.0f, 0.0f}, 1);
	h.Note(1080, Vec3{8.0f, 0.0f, 0.0f}, 1);
	Check(h.At(1020, at) && at.x > 1.99f && at.x < 2.01f, "between two samples, in proportion");
	Check(h.At(1080, at) && at.x == 8.0f, "on the newest, the newest");
	Check(h.At(1300, at) && at.x == 8.0f, "past it, where it was last said to be");
	Check(!h.At(990, at), "before the oldest there is no telling");
	Check(!h.At(1080 + DESYNC_WINDOW_MS + 1, at), "and a probe far past it is on another timeline");
	h.Note(1060, Vec3{100.0f, 0.0f, 0.0f}, 1);
	Check(h.At(1060, at) && at.x > 5.99f && at.x < 6.01f, "an old sample arriving late is dropped");

	for (uint32_t t = 2000; t < 2000 + 40 * 40; t += 40)
		h.Note(t, Vec3{static_cast<float>(t) * 0.01f, 0.0f, 0.0f}, 1);
	Check(h.Count() == PoseHistory::SIZE, "it keeps the last second or so, not everything");
	Check(h.At(3540, at) && at.x > 35.39f && at.x < 35.41f, "and still finds what it kept");

	h.Note(50, Vec3{1.0f, 1.0f, 1.0f}, 2);
	Check(h.Count() == 1 && h.Reporter() == 2, "a new reporter starts a new history on its own clock");

	PoseHistory jump;
	jump.Note(100, Vec3{0.0f, 0.0f, 0.0f}, 1);
	jump.Note(140, Vec3{300.0f, 0.0f, 0.0f}, 1);
	Check(jump.At(110, at) && at.x == 300.0f,
	      "a respawn between two samples is where the copy jumped to, not halfway");
	PoseHistory car(CAR_SNAP_M, true);
	car.Note(100, Vec3{0.0f, 0.0f, 0.0f}, 1, Vec3{100.0f, 0.0f, 0.0f});
	car.Note(180, Vec3{8.0f, 0.0f, 0.0f}, 1, Vec3{100.0f, 0.0f, 0.0f});
	Check(car.At(140, at) && at.x > 3.99f && at.x < 4.01f,
	      "while a car covering 8 m in two snapshots still slides, as its copy does");
	Check(car.At(280, at) && at.x > 17.99f && at.x < 18.01f,
	      "and past its newest it runs on along its velocity, as its copy does");
	Check(car.At(800, at) && at.x > 32.99f && at.x < 33.01f,
	      "for as long as the copy would, and no further");
	PoseHistory taxi(CAR_SNAP_M, false);
	taxi.Note(100, Vec3{0.0f, 0.0f, 0.0f}, 1, Vec3{100.0f, 0.0f, 0.0f});
	Check(taxi.At(300, at) && at.x == 0.0f, "while traffic is held, as its copy is");
	Check(MoveSpeedMps(Vec3{1.0f, 0.0f, 0.0f}).x == 50.0f,
	      "a move speed on the wire is metres a step, fifty steps a second");

	DesyncProbeRow row{};
	row.netId = 9;
	row.atMs  = 50;
	row.pos   = Vec3{4.0f, 5.0f, 1.0f};
	Check(DesyncOffCm(h, row) == 500, "the distance is in centimetres");
	row.atMs = 40;
	Check(DesyncOffCm(h, row) == DESYNC_UNKNOWN, "and unknown where there is no telling");
	row.atMs  = 50;
	row.pos.x = 1.0e9f;
	Check(DesyncOffCm(h, row) == DESYNC_MAX_CM, "a copy at the edge of the world is capped");
}

void TestAProbeIsAnsweredForOthersOnly() {
	std::printf("\na probe is answered for somebody else's things\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Check(s.HistoryFor(alice->netId, bob->id) == &alice->history, "bob asks about alice");
	Check(s.HistoryFor(bob->netId, bob->id) == nullptr, "not about the asker's own player");
	const uint16_t car = Claim(s, *alice, 91)->netId;
	Check(s.HistoryFor(car, bob->id) == &s.FindVehicle(car)->history, "and about a session car");
	Check(s.HistoryFor(4000, bob->id) == nullptr && s.HistoryFor(INVALID_NETID, bob->id) == nullptr,
	      "not about a number the session has nothing under");

	AmbientPed *ped = s.AddPed(alice->id, AmbientPedBody{});
	AmbientCar *taxi = s.AddCar(alice->id, AmbientCarBody{});
	Check(ped && taxi && s.HistoryFor(ped->netId, bob->id) == &ped->history &&
	          s.HistoryFor(taxi->netId, bob->id) == &taxi->history,
	      "and about the pedestrians and traffic alice's game hosts");
	Check(s.HistoryFor(ped->netId, alice->id) == nullptr &&
	          s.HistoryFor(taxi->netId, alice->id) == nullptr,
	      "which alice has no copies of to ask about");
	ped->alive = false;
	Check(s.HistoryFor(ped->netId, bob->id) == nullptr, "and not about a corpse");

	s.FindVehicle(car)->history.Note(1000, Vec3{1.0f, 2.0f, 3.0f}, alice->id);
	s.RemovePeer(1);
	Check(s.FindVehicle(car) && s.FindVehicle(car)->history.Count() == 0,
	      "a leaver's word on where a car was goes with them, clock and all");
}

uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	return a << 24 | b << 16 | c << 8 | d;
}

bool HasLine(const std::vector<std::string> &lines, const char *text) {
	for (const std::string &l : lines)
		if (l.find(text) != std::string::npos)
			return true;
	return false;
}

void TestTheServerSaysHowToReachIt() {
	std::printf("\nthe server says how to reach it\n");
	Check(KindOf(Ip(192, 168, 1, 20)) == AddressKind::Private &&
	          KindOf(Ip(10, 0, 0, 5)) == AddressKind::Private &&
	          KindOf(Ip(172, 16, 0, 1)) == AddressKind::Private &&
	          KindOf(Ip(172, 32, 0, 1)) == AddressKind::Public,
	      "the three private ranges, and not what is next to them");
	Check(KindOf(Ip(100, 64, 0, 1)) == AddressKind::Virtual &&
	          KindOf(Ip(100, 128, 0, 1)) == AddressKind::Public &&
	          KindOf(Ip(169, 254, 3, 4)) == AddressKind::LinkLocal &&
	          KindOf(Ip(127, 0, 0, 1)) == AddressKind::Loopback,
	      "the shared range, link-local and loopback");

	std::vector<std::string> lines = ReachLines({{Ip(192, 168, 1, 20), true}}, 2001);
	Check(HasLine(lines, "players on this network connect to 192.168.1.20:2001"),
	      "a home machine: the address to hand out on the same network");
	Check(HasLine(lines, "UDP port 2001 forwarded on your router to 192.168.1.20"),
	      "and what the router needs for anybody else");
	Check(HasLine(lines, "firewall"), "and the firewall, which is the other half of it");

	lines = ReachLines({{Ip(192, 168, 1, 20), true}, {Ip(10, 0, 0, 5), true},
	                    {Ip(169, 254, 3, 4), false}},
	                   2001);
	Check(HasLine(lines, "192.168.1.20:2001 or 10.0.0.5:2001") && !HasLine(lines, "169.254"),
	      "two networks are both named, and a self-assigned address is not");

	lines = ReachLines({{Ip(172, 25, 160, 1), false}, {Ip(192, 168, 1, 20), true},
	                    {Ip(192, 168, 56, 1), false}},
	                   2001);
	Check(HasLine(lines, "players on this network connect to 192.168.1.20:2001") &&
	          !HasLine(lines, "172.25.160.1") && !HasLine(lines, "192.168.56.1") &&
	          HasLine(lines, "forwarded on your router to 192.168.1.20,"),
	      "a virtual machine's adapter, with no gateway behind it, is left out and "
	      "not forwarded to");
	lines = ReachLines({{Ip(172, 25, 160, 1), false}}, 2001);
	Check(HasLine(lines, "players on this network connect to 172.25.160.1:2001"),
	      "unless it is all there is");

	lines = ReachLines({{Ip(192, 168, 1, 20), true}, {Ip(26, 14, 2, 3), false},
	                    {Ip(25, 1, 2, 3), false}},
	                   2001);
	Check(HasLine(lines, "same VPN or virtual network as this machine connect to "
	                     "26.14.2.3:2001 or 25.1.2.3:2001") &&
	          !HasLine(lines, "public address of its own") && HasLine(lines, "forwarded"),
	      "the addresses the gaming VPNs hand out are not a public address of its own");

	lines = ReachLines({{Ip(203, 0, 113, 9), true}}, 2500);
	Check(HasLine(lines, "public address of its own") && HasLine(lines, "203.0.113.9:2500") &&
	          !HasLine(lines, "forwarded"),
	      "a machine on the internet itself needs no forwarding");

	lines = ReachLines({{Ip(100, 101, 2, 3), false}}, 2001);
	Check(HasLine(lines, "virtual network") && !HasLine(lines, "forwarded"),
	      "a shared address is named for what it is");

	lines = ReachLines({{Ip(100, 72, 5, 6), true}}, 2001);
	Check(HasLine(lines, "carrier's shared address (100.72.5.6:2001)") &&
	          !HasLine(lines, "forwarded") && !HasLine(lines, "virtual network"),
	      "the same range with a gateway behind it is a hotspot, and no forwarding helps");

	lines = ReachLines({{Ip(169, 254, 3, 4), false}}, 2001);
	Check(HasLine(lines, "gave itself (169.254.3.4:2001)") && !HasLine(lines, "could not tell"),
	      "a self-assigned address alone says the network is not there");

	lines = ReachLines({}, 2001);
	Check(HasLine(lines, "could not tell"), "and nothing to list is said too");
}

void TestNetIdsSkipZeroAndLiveOnes() {
	std::printf("\nnetIds skip zero and the ones still in use\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *first = Claim(s, *alice, 91);
	s.NextNetIdForTest(0xFFFF);
	Vehicle *wrap = Claim(s, *alice, 92);
	Check(wrap && wrap->netId == 0xFFFF, "the last number is used");
	Vehicle *after = Claim(s, *alice, 93);
	Check(after && after->netId != INVALID_NETID && after->netId != alice->netId &&
	          after->netId != first->netId,
	      "and after the wrap neither 0 nor anything alive is handed out again");
}

void TestNobodyNearMeansNobodySettlesIt() {
	std::printf("\na leaver's car nobody is near\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");
	s.NotePlayerState(*bob, StandAt(VEHICLE_HANDOVER_RADIUS_M + 5.0f));
	Join(s, 4, "dave");   // on a loading screen: nowhere at all
	s.NotePlayerState(*carol, StandAt(2.0f));
	s.NotePlayerDied(*carol, 17);
	const uint16_t netId = Claim(s, *alice, 91)->netId;

	Check(s.HandOverVehiclesOf(alice->id).empty(),
	      "too far, nowhere, or dead: nobody is handed it");
	s.RemovePeer(1);
	Check(s.CustodianOf(netId) == INVALID_PLAYER &&
	          s.FindVehicle(netId)->driverPlayerId == INVALID_PLAYER,
	      "so it is pinned where it stands, as it always was");

	Player *erin = Join(s, 5, "erin");
	s.NotePlayerState(*erin, StandAt(1.0f));
	Vehicle *wreck = Claim(s, *erin, 91);
	s.NotePlayerState(*bob, StandAt(3.0f));
	s.DestroyVehicle(wreck->netId);
	Check(s.HandOverVehiclesOf(erin->id).empty(), "and a wreck has nothing left to settle");
}

void TestACarsWholeLife() {
	std::printf("\na session car from its claim to its row being reused\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	s.NotePlayerState(*alice, StandAt(0.0f));

	// Claimed and driven somewhere.
	Vehicle *car = Claim(s, *alice, 91);
	const uint16_t first = car->netId;
	s.NoteVehicleState(VehState(first, 300.0f, 1000.0f));
	s.NotePlayerState(*alice, StandAt(290.0f));
	Check(s.ReleaseIdleVehicles(1000).empty(), "a car being driven is kept");

	// Parked, and the two seconds of settling over.
	s.NoteExitVehicle(*alice, first);
	Check(s.ReleaseIdleVehicles(2000).empty(), "a car being settled is kept");
	s.EndCustody(first, alice->id);
	Check(s.ReleaseIdleVehicles(3000).empty(), "a parked car with its driver beside it is kept");

	// Abandoned: she goes a long way off. The minute runs from the last sweep
	// that found her near it, 3000.
	s.NotePlayerState(*alice, StandAt(1500.0f));
	Check(s.ReleaseIdleVehicles(4000).empty(), "and for a minute after she has gone");
	Check(s.ReleaseIdleVehicles(3000 + VEHICLE_RELEASE_MS - 1).empty(),
	      "right up to the minute");
	const std::vector<uint16_t> gone = s.ReleaseIdleVehicles(3000 + VEHICLE_RELEASE_MS);
	Check(gone.size() == 1 && gone[0] == first, "then it is released, by its netId");
	Check(s.FindVehicle(first) == nullptr, "the row is gone");
	Check(s.LiveVehicleCount() == 0, "and nothing is left alive");
	Check(!s.MayReportVehicle(alice->id, first),
	      "a late snapshot for it is refused like any unknown car");
	Player *bob = Join(s, 2, "bob");
	Check(s.BuildBackfill(bob->id, 1).vehicles.empty(), "a joiner isn't told about it");

	// Her next car takes the same row under a new number.
	const size_t rows = s.Vehicles().size();
	Vehicle *next = Claim(s, *alice, 92);
	Check(next != nullptr && s.Vehicles().size() == rows, "the next claim reuses the row");
	Check(next != nullptr && next->netId != first, "under a netId of its own");
	Check(next != nullptr && next->modelId == 92 && next->driverPlayerId == alice->id &&
	          !next->destroyed && next->damagePanels == 0,
	      "and nothing of the old car is left in it");
}

void TestWalkingBackToAParkedCarKeepsIt() {
	std::printf("\nparked, walked 50 m off and back\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteExitVehicle(*alice, netId);
	s.EndCustody(netId, alice->id);

	// Ten minutes of sweeps, once a second, with her 50 m off.
	s.NotePlayerState(*alice, StandAt(50.0f));
	bool kept = true;
	for (uint32_t t = 0; t <= 10 * VEHICLE_RELEASE_MS; t += 1000)
		if (!s.ReleaseIdleVehicles(t).empty())
			kept = false;
	Check(kept && s.FindVehicle(netId) != nullptr,
	      "50 m away, the car is still there after ten minutes");

	// Just inside the radius and just outside it.
	s.NotePlayerState(*alice, StandAt(VEHICLE_KEEP_RADIUS_M - 1.0f));
	Check(s.VehicleNeeded(*car), "inside the keep radius the car is needed");
	s.NotePlayerState(*alice, StandAt(VEHICLE_KEEP_RADIUS_M + 1.0f));
	Check(!s.VehicleNeeded(*car), "outside it, it isn't");

	// Away for most of a minute, back once, away again: the clock restarts.
	uint32_t t = 20 * VEHICLE_RELEASE_MS;
	s.NotePlayerState(*alice, StandAt(10.0f));
	s.ReleaseIdleVehicles(t);
	s.NotePlayerState(*alice, StandAt(900.0f));
	s.ReleaseIdleVehicles(t += VEHICLE_RELEASE_MS - 5000);
	s.NotePlayerState(*alice, StandAt(10.0f));
	s.ReleaseIdleVehicles(t += 1000);
	s.NotePlayerState(*alice, StandAt(900.0f));
	Check(s.ReleaseIdleVehicles(t += VEHICLE_RELEASE_MS - 5000).empty(),
	      "walking back once is enough to start the minute again");
	Check(s.ReleaseIdleVehicles(t += 5000).size() == 1,
	      "and a full minute away after that releases it");
}

void TestNobodyInItOrSettlingItIsReleased() {
	std::printf("\nwhat keeps a car that nobody is near\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	s.NotePlayerState(*alice, StandAt(5000.0f));
	s.NotePlayerState(*bob, StandAt(5000.0f));

	// A passenger, with the driver gone. Everybody's reported position is far
	// off, which is the case of a stale position the seat has to outvote.
	Vehicle *car = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteEnterVehicle(*bob, *car, 1);
	s.NoteExitVehicle(*alice, netId);
	s.EndCustody(netId, alice->id);
	s.ReleaseIdleVehicles(0);
	Check(s.ReleaseIdleVehicles(5 * VEHICLE_RELEASE_MS).empty(),
	      "a passenger keeps it, wherever the session thinks he is");

	// A custodian alone.
	s.NoteEnterVehicle(*bob, *car, 0);
	s.NoteExitVehicle(*bob, netId);
	Check(s.CustodianOf(netId) == bob->id, "bob is settling it");
	Check(s.ReleaseIdleVehicles(10 * VEHICLE_RELEASE_MS).empty(), "a custody keeps it");
	s.EndCustody(netId, bob->id);
	Check(s.ReleaseIdleVehicles(11 * VEHICLE_RELEASE_MS).size() == 1,
	      "and once that ends, a car nobody is near goes");

	// A player the session has no position for yet is near nothing.
	Session u;
	Player *carol = Join(u, 1, "carol");
	Vehicle *c2   = Claim(u, *carol, 91);
	u.NoteExitVehicle(*carol, c2->netId);
	u.EndCustody(c2->netId, carol->id);
	Check(!u.VehicleNeeded(*c2), "no position, no claim on the car");

	// A wreck goes the same way.
	Session w;
	Player *dave = Join(w, 1, "dave");
	Vehicle *c3  = Claim(w, *dave, 91);
	w.DestroyVehicle(c3->netId);
	w.NotePlayerState(*dave, StandAt(1000.0f));
	w.ReleaseIdleVehicles(0);
	Check(w.ReleaseIdleVehicles(VEHICLE_RELEASE_MS).size() == 1, "a wreck is released too");

	// And a session nobody is in lets go of everything.
	Session e;
	Player *ed = Join(e, 1, "ed");
	Claim(e, *ed, 91);
	e.RemovePeer(1);
	e.ReleaseIdleVehicles(0);
	Check(e.ReleaseIdleVehicles(VEHICLE_RELEASE_MS).size() == 1,
	      "an empty session keeps nothing for a minute");
}

void TestTheCapIsOnCarsAliveNotCarsEver() {
	std::printf("\n64 cars at once, not 64 cars a session\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	s.NotePlayerState(*alice, StandAt(0.0f));

	std::vector<uint16_t> ids;
	for (size_t i = 0; i < MAX_SESSION_VEHICLES; ++i) {
		Vehicle *v = Claim(s, *alice, 91);
		if (!v)
			break;
		ids.push_back(v->netId);
		s.NoteExitVehicle(*alice, v->netId);
		s.EndCustody(v->netId, alice->id);
	}
	Check(ids.size() == MAX_SESSION_VEHICLES, "64 claims are granted");
	Check(Claim(s, *alice, 91) == nullptr, "the 65th, with all 64 alive, is not");

	// She leaves them all behind. A minute later the rows are free.
	s.NotePlayerState(*alice, StandAt(3000.0f));
	s.ReleaseIdleVehicles(0);
	Check(s.ReleaseIdleVehicles(VEHICLE_RELEASE_MS).size() == MAX_SESSION_VEHICLES,
	      "all 64 are released");

	// Three hundred more cars over the rest of the session, a few alive at a
	// time, the way somebody who keeps changing cars actually plays.
	uint32_t t     = 2 * VEHICLE_RELEASE_MS;
	int      taken = 0;
	for (int i = 0; i < 300; ++i) {
		s.NotePlayerState(*alice, StandAt(0.0f));
		Vehicle *v = Claim(s, *alice, 91);
		if (!v)
			break;
		++taken;
		for (uint16_t old : ids)
			if (old == v->netId)
				taken = -1000;
		s.NoteExitVehicle(*alice, v->netId);
		s.EndCustody(v->netId, alice->id);
		s.NotePlayerState(*alice, StandAt(3000.0f));
		s.ReleaseIdleVehicles(t);
		t += VEHICLE_RELEASE_MS / 4;
	}
	Check(taken == 300, "300 more claims are all granted, none under an old netId");
	Check(s.Vehicles().size() <= MAX_SESSION_VEHICLES, "and the table never grew past 64");
}

void TestAPromotedCarIsReleasedLikeAnyOther() {
	std::printf("\ntraffic that became a session car goes the same way\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	AmbientCar *traffic = s.AddCar(alice->id, CarBody(91, 5.0f));
	const uint16_t netId = traffic->netId;
	uint8_t        was   = INVALID_PLAYER;
	AmbientCarBody body{};
	Vehicle *v = s.PromoteCar(netId, bob->id, was, body);
	Check(v != nullptr, "bob takes its wheel");
	s.NoteEnterVehicle(*bob, *v, 0);
	s.NoteExitVehicle(*bob, netId);
	s.EndCustody(netId, bob->id);
	s.NotePlayerState(*alice, StandAt(4000.0f));
	s.NotePlayerState(*bob, StandAt(4000.0f));
	s.ReleaseIdleVehicles(0);
	const std::vector<uint16_t> gone = s.ReleaseIdleVehicles(VEHICLE_RELEASE_MS);
	Check(gone.size() == 1 && gone[0] == netId, "released under the number it kept");
	Check(s.FindCar(netId) == nullptr && s.FindVehicle(netId) == nullptr,
	      "and neither roster has it");
}

void TestTheClaimerLeavingDoesntTakeACarSomebodyIsNear() {
	std::printf("\nthe claimer leaves while somebody else is beside the car\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NotePlayerState(*bob, StandAt(20.0f));

	// She drops out at the wheel.
	s.RemovePeer(1);
	Check(car->driverPlayerId == INVALID_PLAYER, "her seat is empty");
	s.ReleaseIdleVehicles(0);
	Check(s.ReleaseIdleVehicles(5 * VEHICLE_RELEASE_MS).empty(),
	      "bob beside it keeps it, for as long as he stays");

	// He walks off.
	s.NotePlayerState(*bob, StandAt(2000.0f));
	Check(s.ReleaseIdleVehicles(6 * VEHICLE_RELEASE_MS - 1).empty(),
	      "not the moment he goes");
	Check(s.ReleaseIdleVehicles(6 * VEHICLE_RELEASE_MS).size() == 1 &&
	          s.FindVehicle(netId) == nullptr,
	      "but a minute after, the car goes too");
}

// ---- money ------------------------------------------------------------------------

MoneyChangeBody Change(uint32_t seq, int32_t delta, int32_t have) {
	MoneyChangeBody b{};
	b.seq   = seq;
	b.delta = delta;
	b.have  = have;
	return b;
}

MoneyAwardBody Award(uint8_t to, int32_t unit, uint8_t keyKind = MONEY_AWARD_UNKEYED,
                     uint16_t keyId = 0) {
	MoneyAwardBody b{};
	b.toPlayerId = to;
	b.kind       = MONEY_AWARD_FIRE;
	b.model      = 90;
	b.unit       = unit;
	b.key.kind   = keyKind;
	b.key.id     = keyId;
	return b;
}

void TestTheMoneyRuleIsOffAndClamped() {
	std::printf("\nthe money rule\n");
	Session s;
	Check(s.MoneyRuleValue() == MONEY_RULE_OFF, "off by default");
	s.SetMoneyRule(MONEY_RULE_SHARED);
	Check(s.MoneyRuleValue() == MONEY_RULE_SHARED, "shared sticks");
	s.SetMoneyRule(7);
	Check(s.MoneyRuleValue() == MONEY_RULE_OFF, "7 is not a rule and means off");
	Check(SaneMoneyRule(MONEY_RULE_OWN) == MONEY_RULE_OWN && SaneMoneyRule(3) == MONEY_RULE_OFF,
	      "and the wire says the same");
}

void TestNothingIsPooledUnlessTheRuleSaysShared() {
	std::printf("\nmoney off or own: no pool\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Check(!s.NoteMoneyChange(alice->id, Change(1, 100, 1100)), "off takes no change");
	s.SetMoneyRule(MONEY_RULE_OWN);
	Check(!s.NoteMoneyChange(alice->id, Change(1, 100, 1100)), "neither does own");
	Check(!s.MoneyPoolSeeded(), "and the pool stays empty");
	const S_Money m = s.MoneyFor(alice->id, INVALID_PLAYER, 0, 5);
	Check(m.rule == MONEY_RULE_OWN && m.flags == 0 && m.total == 0,
	      "what the welcome follow-up says under own: the rule, no pool");
}

void TestTheFirstPlayerInSeedsThePool() {
	std::printf("\na shared wallet\n");
	Session s;
	s.SetMoneyRule(MONEY_RULE_SHARED);
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");

	const S_Money empty = s.MoneyFor(alice->id, INVALID_PLAYER, 0, 5);
	Check(empty.rule == MONEY_RULE_SHARED && (empty.flags & MONEY_POOL_SEEDED) == 0,
	      "an empty pool says so");

	Check(s.NoteMoneyChange(alice->id, Change(1, 0, 5000)) && s.MoneyPoolSeeded() &&
	          s.MoneyPool() == 5000,
	      "alice's $5000 seeds it");
	Check(s.NoteMoneyChange(bob->id, Change(1, 0, 90000)) && s.MoneyPool() == 5000,
	      "bob's seed a moment later is nothing, but still answered so he adopts");

	Check(s.NoteMoneyChange(bob->id, Change(2, 250, 5250)) && s.MoneyPool() == 5250,
	      "bob earns $250 and the pool has it");
	Check(s.NoteMoneyChange(alice->id, Change(2, -1500, 3750)) && s.MoneyPool() == 3750,
	      "alice is busted at six stars, $1500");
	Check(!s.NoteMoneyChange(alice->id, Change(2, -1500, 3750)) && s.MoneyPool() == 3750,
	      "the same change twice is counted once");
	Check(!s.NoteMoneyChange(alice->id, Change(0, 100, 0)), "sequence 0 is nobody's");

	const S_Money toAlice = s.MoneyFor(alice->id, bob->id, 250, 6);
	const S_Money toBob   = s.MoneyFor(bob->id, bob->id, 250, 6);
	Check((toAlice.flags & MONEY_POOL_SEEDED) && toAlice.total == 3750,
	      "everybody is told the same total");
	Check(toAlice.ackSeq == 2 && toBob.ackSeq == 2 && toAlice.fromPlayerId == bob->id,
	      "each with their own last change it holds");

	Check(s.NoteMoneyChange(bob->id, Change(3, -10000, 0)) && s.MoneyPool() == 0,
	      "the pool stops at zero, as the engine's fines do");
	s.NoteMoneyChange(bob->id, Change(4, INT32_MAX, 0));
	s.NoteMoneyChange(bob->id, Change(5, 10, 0));
	Check(s.MoneyPool() == INT32_MAX, "and at the top of an int32");
}

void TestTheWalletEmptiesWithTheSession() {
	std::printf("\nthe wallet and the session\n");
	Session s;
	s.SetMoneyRule(MONEY_RULE_SHARED);
	Player *alice = Join(s, 1, "alice");
	Join(s, 2, "bob");
	s.NoteMoneyChange(alice->id, Change(1, 0, 700));
	s.RemovePeer(1);
	Check(s.MoneyPoolSeeded() && s.MoneyPool() == 700, "one player leaving takes nothing");
	s.RemovePeer(2);
	Check(!s.MoneyPoolSeeded(), "the last one out empties it");

	Player *carl = Join(s, 3, "carl");
	Check(carl->moneySeq == 0, "a new player in an old slot starts his count again");
	s.NoteMoneyChange(carl->id, Change(1, 0, 42));
	Check(s.MoneyPool() == 42, "and the next one in seeds it afresh");

	s.SetMoneyRule(MONEY_RULE_SHARED);
	Check(s.MoneyPool() == 42, "setting the same rule again keeps it");
	s.SetMoneyRule(MONEY_RULE_OWN);
	s.SetMoneyRule(MONEY_RULE_SHARED);
	Check(!s.MoneyPoolSeeded(), "changing the rule empties it");
}

void TestAnAwardIsDeliveredOncePerCar() {
	std::printf("\nmoney awards\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Check(!s.TakeMoneyAward(alice->id, Award(bob->id, 50), 1000),
	      "nothing is delivered with money off");

	s.SetMoneyRule(MONEY_RULE_OWN);
	Check(s.TakeMoneyAward(alice->id, Award(bob->id, 50), 1000),
	      "own: alice's game pays bob for a car only it decided");
	Check(s.TakeMoneyAward(alice->id, Award(bob->id, 50), 1001),
	      "and a second car like it, a moment later, is paid as well");
	Check(!s.TakeMoneyAward(alice->id, Award(9, 50), 1000), "nobody in slot 9");
	Check(!s.TakeMoneyAward(alice->id, Award(bob->id, 0), 1000), "nothing for a car worth $0");
	Check(!s.TakeMoneyAward(alice->id, Award(bob->id, MONEY_AWARD_MAX_UNIT + 1), 1000),
	      "nor for more than any car is worth");
	MoneyAwardBody odd = Award(bob->id, 50);
	odd.kind = 7;
	Check(!s.TakeMoneyAward(alice->id, odd, 1000), "nor from a caller the engine hasn't got");

	Check(s.TakeMoneyAward(alice->id, Award(bob->id, 50, UNOWNED_PARKED, 17), 2000),
	      "a parked car: alice's game is first");
	Check(!s.TakeMoneyAward(bob->id, Award(bob->id, 50, UNOWNED_PARKED, 17), 2040),
	      "bob's own game decided it too and is not paid again");
	Check(s.TakeMoneyAward(bob->id, Award(bob->id, 50, UNOWNED_PARKED, 18), 2040),
	      "the car next to it is another car");
	Check(s.TakeMoneyAward(bob->id, Award(alice->id, 50, UNOWNED_SESSION, 17), 2040),
	      "and a session car with the same number is another kind of name");
	Check(s.TakeMoneyAward(alice->id,
	                       Award(bob->id, 50, UNOWNED_PARKED, 17),
	                       2000 + MONEY_AWARD_KEY_MS),
	      "the generator's next car, ten seconds on, is paid for");
	Check(s.TakeMoneyAward(alice->id, Award(alice->id, 50, UNOWNED_AMBIENT, 17), 2000),
	      "traffic has one host and is never keyed, so it is not deduped");
}

// tools/sessiontest/rampagevote.cpp
int RunRampageVoteTests();
int RunAdoptTests();

int main() {
	g_failures += RunRampageVoteTests();
	g_failures += RunAdoptTests();
	TestAJoinerIsToldWhatEverybodyIsWearing();
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
	TestAnArrestIsARespawnWithoutADeath();
	TestABackfilledCarCarriesItsCondition();
	TestAWreckIsNotBackfilled();
	TestADriverLeavingReleasesTheirCar();
	TestAPassengerIsBackfilledInTheirOwnSeat();
	TestOnlyTheDriverMayReportTheCar();
	TestAPassengerGetsOutOfAWreck();
	TestSteppingStraightFromOneCarIntoAnother();
	TestAJackTakesTheCarOffThePlayerWhoHadIt();
	TestTakingAnEmptySeatIsNotAJack();
	TestAJackedCarKeepsItsPassengers();
	TestTheJoinerIsNotInTheirOwnBackfill();

	TestOnlyTheOwnerMayTakeAPedAway();
	TestEveryPedGetsItsOwnName();
	TestAPlayerTakesTheirPedsWithThem();
	TestABackfilledPedIsNeverMistakenForYourOwn();
	TestOnlyTheOwnerMayKillTheirPed();
	TestOnlySomebodyElseMayShootYourPed();
	TestAPedestriansRoundsAndHitsAreHisHosts();
	TestFriendlyFireHasNoSayOverPedestrians();
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
	TestACollectedDropIsForgotten();
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
	TestAHitOnACarIsRoutedToItsDriver();
	TestNobodyMayShootACarNobodyIsDriving();
	TestAHitOnACarInCustodyGoesToTheCustodian();
	TestACustodyEndingMovesTheHitsOn();
	TestOnlyTheCustodianWritesOffACarInCustody();
	TestAHitOnACarNobodyHoldsMakesTheShooterItsCustodian();
	TestAWreckedCarTakesNoMoreHits();
	TestAPassengerMayNotShootTheCarHeIsSittingIn();
	TestFriendlyFireHasNoSayOverCars();
	TestAHitIsTheInverseOfEveryOtherCarPacket();
	TestAHitOnTrafficIsRoutedToItsHost();
	TestTrafficThatIsGoneTakesNoHits();
	TestTrafficAndDrivenHitsDoNotCross();
	TestARespraysClearTravelsAsADamageReport();

	TestGettingOutHandsTheCarToTheDriverWhoLeftIt();
	TestOnlyTheCustodianCanHandACarBack();
	TestADriverEndsACustody();
	TestAPassengerDoesNotEndACustody();
	TestACustodianLeavingGivesTheCarBackToNobody();
	TestAWreckIsNotHandedToAnybodyToSettle();
	TestTheCustodianReportsTheDentsOfASettle();
	TestADentAfterACustodyEndsIsRefused();
	TestALaterDriversLighterDentIsStillNews();
	TestGettingIntoSomebodyElsesTrafficMovesOwnership();
	TestAPromotionNeedsATrafficCarThatIsActuallyThere();
	TestAJoinerIsToldWhatShapeTheCarsAreIn();
	TestAJoinerIsToldWhichParkedCarsAreWrecks();
	TestAnUnownedWreckStopsBeingBackfilledAfterAMinute();

	TestAmmoSyncIsOffUntilTheServerSaysOtherwise();
	TestOnlyAChangeIsWorthRelaying();
	TestAJoinerIsToldWhatEveryoneIsCarrying();
	TestLosingAWeaponIsNewsAndThenForgotten();
	TestNothingIsBackfilledWithTheSwitchOff();

	TestOneFrenzyAtATimeAndEverybodyIsPutInIt();
	TestAJoinerIsPutIntoTheRampageAlreadyRunning();
	TestAKillForAnOldFrenzyIsNotCountedAgainstTheNewOne();
	TestTheFirstEndingWins();
	TestTheServerEndsARampageNobodyIsLeftToEnd();
	TestARampageWithNoTimeLimitNeverTimesOut();
	TestTheTargetIsScaledByThePlayerCount();
	TestWithRampagesOffThereIsNoSessionFrenzy();
	TestANamedCarCountsOnceHoweverManyMachinesSawIt();
	TestACarForAnOldFrenzyIsDropped();
	TestTheOldestCarNameGoesFirst();

	TestAStreetObjectRelayIsStampedAndPassedOn();

	TestAHelicopterStateIsRelayedUntilItIsGone();
	TestAHelicopterIsGoneOnce();
	TestAHitGoesToTheOwnerOfALiveHelicopter();
	TestAHelicopterRoundIsRelayedOnlyFromALiveOne();
	TestOnlySomebodyElseInTheSessionIsCredited();
	TestALeavingOwnerTakesHisHelicoptersWithHim();

	TestEveryCheatIsRelayedWhereItsRouteSays();
	TestTheSessionRemembersWhatEverybodyRuns();
	TestTheCheatRuleIsClamped();

	TestACarsWholeLife();
	TestWalkingBackToAParkedCarKeepsIt();
	TestNobodyInItOrSettlingItIsReleased();
	TestTheCapIsOnCarsAliveNotCarsEver();
	TestAPromotedCarIsReleasedLikeAnyOther();
	TestTheClaimerLeavingDoesntTakeACarSomebodyIsNear();
	TestTheMoneyRuleIsOffAndClamped();
	TestNothingIsPooledUnlessTheRuleSaysShared();
	TestTheFirstPlayerInSeedsThePool();
	TestTheWalletEmptiesWithTheSession();
	TestAnAwardIsDeliveredOncePerCar();
	TestATrafficCarsDentsComeFromItsHost();
	TestALeaversCarGoesToWhoeverIsNextToIt();
	TestAPackageIsEverybodysByDefault();
	TestAPackageIsYourOwnUnderPerPlayer();
	TestNetIdsSkipZeroAndLiveOnes();
	TestNobodyNearMeansNobodySettlesIt();
	TestAJoinerAndANewDriverGetTheDents();
	TestTheHistoryIsWhereTheOwnerSaidItWas();
	TestAProbeIsAnsweredForOthersOnly();
	TestTheServerSaysHowToReachIt();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
