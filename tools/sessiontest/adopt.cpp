// A leaver's crowd, the server's half: server/core/adopt.h and
// Session::HandOverAmbientOf. Who takes what, how far is too far, and that a
// car never leaves without the people sitting in it.

#include "adopt.h"
#include "session.h"

#include <cstdio>

using namespace coopiii;

namespace {

int g_adoptFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_adoptFailures;
}

AdoptViewer At(uint8_t id, float x, float y = 0.0f, float z = 0.0f) {
	AdoptViewer v;
	v.playerId = id;
	v.pos      = {x, y, z};
	return v;
}

AdoptPed Walker(uint16_t netId, float x, uint8_t pedType = AMBIENT_PEDTYPE_CIVMALE) {
	AdoptPed p;
	p.netId   = netId;
	p.pos     = {x, 0.0f, 0.0f};
	p.pedType = pedType;
	return p;
}

AdoptPed Sitting(uint16_t netId, uint16_t car, float x,
                 uint8_t pedType = AMBIENT_PEDTYPE_CIVFEMALE) {
	AdoptPed p = Walker(netId, x, pedType);
	p.vehicleNetId = car;
	return p;
}

AdoptCar Car(uint16_t netId, float x, bool destroyed = false) {
	AdoptCar c;
	c.netId     = netId;
	c.pos       = {x, 0.0f, 0.0f};
	c.destroyed = destroyed;
	return c;
}

const AdoptVerdict *Find(const std::vector<AdoptVerdict> &v, uint16_t netId) {
	for (const AdoptVerdict &x : v)
		if (x.netId == netId)
			return &x;
	return nullptr;
}

uint8_t AdopterOf(const std::vector<AdoptVerdict> &v, uint16_t netId) {
	const AdoptVerdict *x = Find(v, netId);
	return x ? x->adopter : static_cast<uint8_t>(0xFE);
}

void TestTheDistancesAreTheEnginesWidest() {
	std::printf("\nhow far is too far, from the engine's own reapers\n");
	Check(AMBIENT_CAR_ADOPT_RADIUS_M == 195.0f,
	      "a car: 130 m on screen times 1.5 for an extended-range car");
	Check(AMBIENT_PED_ADOPT_RADIUS_M == 97.5f,
	      "a ped: 65 m for one that is never culled early, times the 1.5 clamp");
}

void TestTheNearestPlayerTakesIt() {
	std::printf("\nthe nearest remaining player takes it\n");
	const AdoptViewer two[] = {At(1, 50.0f), At(2, -20.0f)};
	Check(NearestAdopter(Vec3{0, 0, 0}, 100.0f, two, 2) == 2, "the nearer of two");
	Check(NearestAdopter(Vec3{0, 0, 0}, 10.0f, two, 2) == INVALID_PLAYER,
	      "nobody inside the radius, nobody");
	Check(NearestAdopter(Vec3{0, 0, 0}, 20.0f, two, 2) == 2,
	      "exactly on the radius is still inside it");
	Check(NearestAdopter(Vec3{0, 0, 0}, 100.0f, two, 0) == INVALID_PLAYER,
	      "nobody left in the session, nobody");

	const AdoptViewer tied[] = {At(3, 30.0f), At(4, -30.0f)};
	Check(NearestAdopter(Vec3{0, 0, 0}, 100.0f, tied, 2) == 3,
	      "a tie goes to the first, which is the lower slot");

	// The engine measures on the ground: a player on a roof above the ped is
	// as near as one standing beside him.
	const AdoptViewer roof[] = {At(5, 40.0f, 0.0f, 0.0f), At(6, 10.0f, 0.0f, 200.0f)};
	Check(NearestAdopter(Vec3{0, 0, 0}, 50.0f, roof, 2) == 6, "in 2D, as the reapers measure");
}

void TestAPedestrianOnFoot() {
	std::printf("\na pedestrian on foot\n");
	const std::vector<AdoptViewer> bob = {At(1, 0.0f)};
	const auto v = PlanAmbientHandover({}, {Walker(10, 90.0f), Walker(11, 100.0f)}, bob);
	Check(AdopterOf(v, 10) == 1, "at 90 m he is bob's");
	Check(AdopterOf(v, 11) == INVALID_PLAYER, "at 100 m nobody's engine would keep him");

	AdoptPed dead = Walker(12, 1.0f);
	dead.alive = false;
	const auto w = PlanAmbientHandover(
	    {}, {dead, Walker(13, 1.0f, 6), Walker(14, 1.0f, 7), Walker(15, 1.0f, 18),
	         Walker(16, 1.0f, AMBIENT_PEDTYPE_CIVFEMALE)}, bob);
	Check(AdopterOf(w, 12) == INVALID_PLAYER, "a corpse is let go of, whoever is next to it");
	Check(AdopterOf(w, 13) == INVALID_PLAYER, "and so is a cop - his replica is no CCopPed");
	Check(AdopterOf(w, 14) == INVALID_PLAYER && AdopterOf(w, 15) == INVALID_PLAYER,
	      "and a gang member or a criminal, built here as a plain civilian");
	Check(AdopterOf(w, 16) == 1, "a civilian woman is taken");
}

void TestACarGoesWithItsOccupants() {
	std::printf("\na car and whoever is in it go together\n");
	const std::vector<AdoptViewer> viewers = {At(1, 0.0f), At(2, 300.0f)};
	const auto v = PlanAmbientHandover({Car(50, 160.0f)},
	                                   {Walker(20, 5.0f), Sitting(21, 50, 160.0f),
	                                    Sitting(22, 50, 160.0f)},
	                                   viewers);
	Check(AdopterOf(v, 50) == 2, "the car is carol's, 140 m from her, inside 195");
	Check(AdopterOf(v, 21) == 2 && AdopterOf(v, 22) == 2,
	      "its driver and passenger go with it, past a pedestrian's own reach");
	Check(v.size() == 4 && v[0].netId == 50 && v[1].group == v[0].group &&
	          v[2].group == v[0].group && v[3].netId == 20,
	      "out as the car, its people, then the peds on foot");
	Check(AdopterOf(v, 20) == 1, "and the man on the pavement is bob's");

	const auto cops = PlanAmbientHandover({Car(51, 1.0f)},
	                                      {Sitting(23, 51, 1.0f, 6), Sitting(24, 51, 1.0f)},
	                                      viewers);
	Check(AdopterOf(cops, 51) == INVALID_PLAYER && AdopterOf(cops, 23) == INVALID_PLAYER &&
	          AdopterOf(cops, 24) == INVALID_PLAYER,
	      "one occupant nobody can take and the whole car goes, not an empty police car");

	AdoptPed corpse = Sitting(25, 52, 1.0f);
	corpse.alive = false;
	const auto hearse = PlanAmbientHandover({Car(52, 1.0f)}, {corpse}, viewers);
	Check(AdopterOf(hearse, 52) == INVALID_PLAYER, "nor a car with a dead man at the wheel");

	const auto wreck = PlanAmbientHandover({Car(53, 1.0f, true)}, {Sitting(26, 53, 1.0f)},
	                                       viewers);
	Check(AdopterOf(wreck, 53) == INVALID_PLAYER && AdopterOf(wreck, 26) == INVALID_PLAYER,
	      "a wreck goes, and whoever the host still had in it");

	const auto parked = PlanAmbientHandover({Car(54, 10.0f)}, {}, viewers);
	Check(AdopterOf(parked, 54) == 1, "an empty car is handed on like any other");

	const auto elsewhere =
	    PlanAmbientHandover({}, {Sitting(27, 999, 10.0f)}, viewers);
	Check(AdopterOf(elsewhere, 27) == 1,
	      "a ped in a car that is not the leaver's is decided as a pedestrian");

	const auto alone = PlanAmbientHandover({Car(55, 1.0f)}, {Walker(28, 1.0f)}, {});
	Check(AdopterOf(alone, 55) == INVALID_PLAYER && AdopterOf(alone, 28) == INVALID_PLAYER,
	      "the last player out takes everything with him");
}

void TestAPacketNeverSplitsACar() {
	std::printf("\npacked for the wire\n");
	std::vector<AdoptCar>    cars;
	std::vector<AdoptPed>    peds;
	uint16_t                 next = 100;
	for (uint16_t c = 0; c < 5; ++c) {
		const uint16_t car = static_cast<uint16_t>(900 + c);
		cars.push_back(Car(car, 1.0f));
		for (int i = 0; i < 8; ++i)
			peds.push_back(Sitting(next++, car, 1.0f));
	}
	for (int i = 0; i < 20; ++i)
		peds.push_back(Walker(next++, 1.0f));
	peds.push_back(Walker(next++, 1000.0f));   // nobody near: not in any packet

	const auto verdicts = PlanAmbientHandover(cars, peds, {At(3, 0.0f)});
	const auto batches  = PackAdoptRows(verdicts);

	size_t rows = 0;
	bool   fits = true, whole = true;
	for (const auto &b : batches) {
		rows += b.size();
		fits  = fits && !b.empty() && b.size() <= MAX_ADOPT_ROWS;
		// A car row must be followed by its eight inside the same batch.
		for (size_t i = 0; i < b.size(); ++i) {
			if (b[i].kind != AMBIENT_ADOPT_CAR)
				continue;
			if (i + 8 >= b.size()) {
				whole = false;
				continue;
			}
			for (size_t k = i + 1; k <= i + 8; ++k)
				whole = whole && b[k].kind == AMBIENT_ADOPT_PED;
		}
	}
	Check(rows == 5 * 9 + 20, "every adopted row, and not the one nobody takes");
	Check(fits, "no batch over the packet's 32");
	Check(whole, "and no car split from the people in it");
	Check(batches.size() == 3, "three batches: 27, then 18 + 14, then the last 6");
	Check(batches[0][0].newOwnerPlayerId == 3 && batches[0][0].netId == 900 &&
	          batches[0][0].kind == AMBIENT_ADOPT_CAR,
	      "each row names the adopter");
	Check(PackAdoptRows({}).empty(), "nothing adopted, nothing sent");
}

// ---- the session applying it ------------------------------------------------

Player *Join(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

void Stand(Session &s, Player &p, float x) {
	PlayerStateBody b{};
	b.pos    = {x, 0.0f, 0.0f};
	b.health = 100.0f;
	s.NotePlayerState(p, b);
}

AmbientPedBody PedBody(float x, uint8_t pedType = AMBIENT_PEDTYPE_CIVMALE) {
	AmbientPedBody b{};
	b.modelId = 7;
	b.pedType = pedType;
	b.pos     = {x, 0.0f, 0.0f};
	return b;
}

AmbientCarBody CarBody(float x) {
	AmbientCarBody b{};
	b.modelId = 91;
	b.extra1 = b.extra2 = -1;
	b.pos = {x, 0.0f, 0.0f};
	b.rot = {0.0f, 0.0f, 0.0f, 1.0f};
	return b;
}

AmbientPedState PedRow(uint16_t netId, float x, uint16_t car = INVALID_NETID) {
	AmbientPedState r{};
	r.netId        = netId;
	r.vehicleNetId = car;
	r.pos          = {x, 0.0f, 0.0f};
	return r;
}

void TestTheSessionHandsTheCrowdOn() {
	std::printf("\nthe session hands a leaver's crowd on\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Player *carol = Join(s, 3, "carol");
	Player *dave  = Join(s, 4, "dave");
	Stand(s, *alice, 0.0f);
	Stand(s, *bob, 10.0f);
	Stand(s, *carol, 400.0f);
	Stand(s, *dave, 5.0f);
	s.NotePlayerDied(*dave, 17);   // nearest of all, and on the floor

	const uint16_t near    = s.AddPed(alice->id, PedBody(20.0f))->netId;
	const uint16_t far     = s.AddPed(alice->id, PedBody(-200.0f))->netId;
	const uint16_t byCarol = s.AddPed(alice->id, PedBody(390.0f))->netId;
	const uint16_t cop     = s.AddPed(alice->id, PedBody(12.0f, 6))->netId;
	const uint16_t car     = s.AddCar(alice->id, CarBody(30.0f))->netId;
	const uint16_t driver  = s.AddPed(alice->id, PedBody(30.0f))->netId;
	const uint16_t bobs    = s.AddPed(bob->id, PedBody(11.0f))->netId;

	// The host says his driver sits in his car. The same row that says it
	// moves him; nothing else tells the session.
	Check(s.NotePedState(PedRow(driver, 30.0f, car), alice->id), "alice streams her driver");
	s.NoteCarState(AmbientCarState{car, 0, {35.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {}}, alice->id);

	const std::vector<AdoptVerdict> v = s.HandOverAmbientOf(alice->id);
	Check(v.size() == 6, "all six of hers decided, and none of bob's own");
	Check(s.FindPed(near) && s.FindPed(near)->ownerPlayerId == bob->id,
	      "the ped beside bob is his - dave is nearer, but dead");
	Check(s.FindPed(byCarol) && s.FindPed(byCarol)->ownerPlayerId == carol->id,
	      "the one down carol's street is hers");
	Check(s.FindPed(far) == nullptr, "the one nobody is near is gone from the session");
	Check(s.FindPed(cop) == nullptr, "and the cop, whoever is near him");
	Check(s.FindCar(car) && s.FindCar(car)->ownerPlayerId == bob->id &&
	          s.FindPed(driver) && s.FindPed(driver)->ownerPlayerId == bob->id,
	      "her car goes to bob with its driver in it");
	Check(s.FindPed(bobs)->ownerPlayerId == bob->id, "bob's own crowd is untouched");
	Check(s.PedsOwnedBy(alice->id).empty() && s.CarsOwnedBy(alice->id).empty(),
	      "nothing is left filed under alice");

	Check(!s.NotePedState(PedRow(near, 21.0f), alice->id),
	      "a row from alice that was still in flight is refused");
	Check(s.NotePedState(PedRow(near, 22.0f), bob->id) &&
	          s.FindPed(near)->body.pos.x == 22.0f,
	      "and bob's rows are taken from here on");
	Check(s.PedDamageRecipient(near, carol->id) == bob,
	      "a hit on him goes to his new host");
	Check(s.NotePedDeath(PedDeathBody{near, 0}, bob->id), "whose word on his death counts");
	Check(!s.RemoveCar(car, alice->id) && s.RemoveCar(car, bob->id),
	      "and whose despawn of the car is the one that is taken");

	s.RemovePeer(1);
	const Backfill b = s.BuildBackfill(bob->id, 0);
	bool named = false;
	for (const S_PedSpawn &p : b.peds)
		if (p.netId == byCarol)
			named = p.ownerPlayerId == carol->id;
	Check(named, "somebody joining later is told the new owner");
}

void TestAHostOfNothingHandsOnNothing() {
	std::printf("\nnothing to hand on\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Check(s.HandOverAmbientOf(alice->id).empty(), "a player who hosted nothing");
	Check(s.HandOverAmbientOf(INVALID_PLAYER).empty(), "and no player at all");
}

} // namespace

int RunAdoptTests() {
	TestTheDistancesAreTheEnginesWidest();
	TestTheNearestPlayerTakesIt();
	TestAPedestrianOnFoot();
	TestACarGoesWithItsOccupants();
	TestAPacketNeverSplitsACar();
	TestTheSessionHandsTheCrowdOn();
	TestAHostOfNothingHandsOnNothing();
	return g_adoptFailures;
}
