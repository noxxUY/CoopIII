// Cheats in a session: the pure half of game/cheats.h, the routing through
// Client, the shared wreck queue BANGBANGBANG overflowed, and - when a copy of
// the retail exe is handed over - the cheat table read back out of it.
//
// Nothing here types a key into a game. What runs is every decision in front
// of the engine: which cheat a buffer completes, where each one is allowed to
// run, what a receiver calls to arrive where the typist's machine was left,
// and that the car refusals BANGBANGBANG runs into are decided on the car.

#include "client.h"
#include "game/cheats.h"
#include "game/vehicle.h"
#include "game/wreckqueue.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_cheatFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_cheatFailures;
}

// What the player types: the table holds it backwards, the way it is compared.
std::string Forward(uint8_t id) {
	const std::string reversed(CHEAT_SITES[id].reversed);
	return std::string(reversed.rbegin(), reversed.rend());
}

// A buffer the way the game starts with it: zeros, from .bss. Nothing ever
// fills it with anything else, which is the whole of why BOOOOORING works
// only as the first thing typed (addresses.h, CHEAT_SITES).
struct Keyboard {
	char buffer[KEYBOARD_CHEAT_STRING_LEN];
	Keyboard() { std::memset(buffer, 0, sizeof(buffer)); }

	// Types one key, and returns what it fired.
	std::vector<uint8_t> Key(char c) {
		PushCheatChar(buffer, c);
		uint8_t       ids[CHEAT_COUNT];
		const uint8_t n = MatchTypedCheats(buffer, ids, CHEAT_COUNT);
		return std::vector<uint8_t>(ids, ids + n);
	}
};

// ---- the table ---------------------------------------------------------------

void TestTheTableIsWhatTheEngineCompares() {
	std::printf("\nthe cheat table\n");

	bool lengths = true, unique = true, bounded = true;
	for (uint8_t a = 0; a < CHEAT_COUNT; ++a) {
		const CheatSite &s = CHEAT_SITES[a];
		const size_t     n = std::strlen(s.reversed);
		if (a == CHEAT_SLOW_TIME ? !(n == 10 && s.length == 16) : n != s.length)
			lengths = false;
		if (s.length > KEYBOARD_CHEAT_STRING_LEN)
			bounded = false;
		for (uint8_t b = a + 1; b < CHEAT_COUNT; ++b)
			if (s.handler == CHEAT_SITES[b].handler ||
			    s.string == CHEAT_SITES[b].string ||
			    std::strcmp(s.reversed, CHEAT_SITES[b].reversed) == 0)
				unique = false;
	}
	Check(lengths, "every row compares its own length, except BOOOOORING's 16 for ten letters");
	Check(bounded, "and fits the twenty-byte buffer");
	Check(unique, "no two rows share a string, an address or a handler");

	Check(Forward(CHEAT_WANTED_UP) == "MOREPOLICEPLEASE",
	      "row 3 is what the user types for two more stars");
	Check(Forward(CHEAT_ARMOUR) == "TURTOISE", "the armour row is the 1.0 spelling");
	Check(Forward(CHEAT_BLOW_UP_CARS) == "BANGBANGBANG", "row 6 blows up the cars");
	Check(Forward(CHEAT_FOGGY) == "PEASOUP", "row 17 is fog");
	Check(Forward(CHEAT_NASTY_LIMBS) == "NASTYLIMBSCHEAT", "and the last is the one that does nothing");
}

// ---- the matcher -------------------------------------------------------------

void TestEachCheatFiresOnItsLastKeyAndOnlyThen() {
	std::printf("\ntyping every cheat\n");
	int wrong = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		Keyboard          kb;
		const std::string word = Forward(id);
		for (size_t i = 0; i < word.size(); ++i) {
			const std::vector<uint8_t> fired = kb.Key(word[i]);
			const bool last = i + 1 == word.size();
			if (last ? (fired.size() != 1 || fired[0] != id) : !fired.empty()) {
				++wrong;
				std::printf("    %s: key %zu fired %zu cheat(s)\n", word.c_str(), i,
				            fired.size());
			}
		}
	}
	Check(wrong == 0, "all 23 fire exactly once, on the last key, as themselves");
}

// The function is 23 separate ifs, so a cheat whose spelling ends another's
// would fire both. None does, and this is what says so rather than an
// assumption: every cheat typed straight after every other, with no pause.
//
// Two cheats do fire more than twice when typed twice, and only as
// themselves: GUNSGUNSGUNS and BANGBANGBANG repeat every four letters, so
// every further GUNS or BANG completes them again. That is the engine's own
// behaviour - the same buffer, the same compare - and single player has it.
void TestNoCheatFiresAnotherOnTheWay() {
	std::printf("\nevery cheat straight after every other\n");
	int wrong = 0;
	for (uint8_t a = 0; a < CHEAT_COUNT; ++a) {
		for (uint8_t b = 0; b < CHEAT_COUNT; ++b) {
			Keyboard          kb;
			const std::string both = Forward(a) + Forward(b);
			std::vector<uint8_t> all;
			for (char c : both) {
				const std::vector<uint8_t> f = kb.Key(c);
				all.insert(all.end(), f.begin(), f.end());
			}
			// What the engine would fire: A, then B - unless B is BOOOOORING,
			// which A's last letter now sits in the way of - and for the two
			// periodic cheats typed twice, the two extra completions.
			std::vector<uint8_t> want{a};
			if (b != CHEAT_SLOW_TIME)
				want.push_back(b);
			if (a == b && (a == CHEAT_WEAPONS || a == CHEAT_BLOW_UP_CARS))
				want.insert(want.end(), {a, a});
			const bool right = all == want;
			if (!right) {
				++wrong;
				std::printf("    %s then %s fired %zu:", Forward(a).c_str(),
				            Forward(b).c_str(), all.size());
				for (uint8_t id : all)
					std::printf(" %s", Forward(id).c_str());
				std::printf("\n");
			}
		}
	}
	Check(wrong == 0, "529 pairs: nothing ever fires a cheat that was not typed");

	Keyboard kb;
	int      fired = 0;
	for (char c : std::string("BANGBANGBANGBANGBANG"))
		fired += int(kb.Key(c).size());
	Check(fired == 3, "BANGBANGBANG then BANG, BANG fires three times, as in single player");
}

// The retail bug, kept. Sixteen compared for ten letters means the eleventh
// has to be the zero the buffer started with.
void TestBoooooringOnlyWorksAsTheFirstThingTyped() {
	std::printf("\nBOOOOORING\n");
	{
		Keyboard kb;
		int      fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 1, "typed first, it fires");
	}
	{
		Keyboard kb;
		kb.Key('X');
		int fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 0, "after one other key, never - which is 1.0 exactly");
	}
	{
		Keyboard kb;
		for (char c : std::string("PEASOUP"))
			kb.Key(c);
		int fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 0, "after another cheat, never");
	}
}

void TestTheBufferKeepsTheLastTwentyNewestFirst() {
	std::printf("\nthe buffer\n");
	Keyboard kb;
	for (char c = 'A'; c <= 'Y'; ++c)   // 25 keys
		kb.Key(c);
	Check(kb.buffer[0] == 'Y' && kb.buffer[19] == 'F',
	      "twenty-five keys leave the last twenty, newest at [0]");
}

// ---- the classification --------------------------------------------------------

void TestWhatEachCheatTouches() {
	std::printf("\nwho each cheat belongs to\n");

	const uint8_t world[] = {CHEAT_BLOW_UP_CARS, CHEAT_MAYHEM, CHEAT_WEAPONS_FOR_ALL,
	                         CHEAT_FAST_TIME,    CHEAT_SLOW_TIME, CHEAT_SUNNY,
	                         CHEAT_CLOUDY,       CHEAT_RAINY,  CHEAT_FOGGY,
	                         CHEAT_FAST_WEATHER};
	int worldCount = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (CheatChangesTheWorld(id))
			++worldCount;
	bool listed = true;
	for (uint8_t id : world)
		if (!CheatChangesTheWorld(id))
			listed = false;
	Check(worldCount == 10 && listed, "exactly the ten world cheats change the world");
	Check(!CheatChangesTheWorld(CHEAT_EVERYBODY_ATTACKS),
	      "NOBODYLIKESME is the typist's: PLAYER1 is the local player everywhere");
	Check(!CheatChangesTheWorld(CHEAT_TANK) && !CheatChangesTheWorld(CHEAT_WANTED_UP),
	      "the tank and the stars are the typist's own");

	bool skies = true;
	for (uint8_t id = CHEAT_SUNNY; id <= CHEAT_FOGGY; ++id)
		if (CheatRouteOf(id) != CHEAT_ROUTE_HOST)
			skies = false;
	Check(skies, "the four skies go to the host");
	Check(CheatRouteOf(CHEAT_MAYHEM) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_WEAPONS_FOR_ALL) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_FAST_TIME) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_SLOW_TIME) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_FAST_WEATHER) == CHEAT_ROUTE_EVERYONE,
	      "the clock's speed, the riot and the armed crowd run everywhere");
	Check(CheatRouteOf(CHEAT_BLOW_UP_CARS) == CHEAT_ROUTE_LOCAL,
	      "BANGBANGBANG runs where it was typed; its wrecks are what travel");

	// Anything routed has to be a world cheat, or `personal` would let it
	// reach other machines.
	bool routedIsWorld = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (CheatRouteOf(id) != CHEAT_ROUTE_LOCAL && !CheatChangesTheWorld(id))
			routedIsWorld = false;
	Check(routedIsWorld, "nothing personal is ever routed");
}

void TestTheRuleDecidesWhatIsAllowed() {
	std::printf("\nthe server's rule\n");
	int shared = 0, personal = 0, off = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		shared += CheatAllowed(CHEAT_RULE_SHARED, id);
		personal += CheatAllowed(CHEAT_RULE_PERSONAL, id);
		off += CheatAllowed(CHEAT_RULE_OFF, id);
	}
	Check(shared == 23, "shared allows all 23");
	Check(personal == 13, "personal allows the 13 about the typist");
	Check(off == 0, "off allows none");
	Check(!CheatAllowed(CHEAT_RULE_SHARED, CHEAT_COUNT), "a cheat that does not exist is never allowed");

	const uint8_t others = SESSION_FRIENDLY_FIRE | SESSION_AMMO_SYNC |
	                       SESSION_WANTED_MASK | SESSION_RAMPAGE_MASK;
	bool roundTrip = true;
	for (uint8_t rule = 0; rule <= CHEAT_RULE_OFF; ++rule) {
		const uint8_t flags = FlagsWithCheatRule(others, rule);
		if (CheatRuleFromFlags(flags) != rule || (flags & others) != others)
			roundTrip = false;
	}
	Check(roundTrip, "the rule rides bits 6-7 and leaves the other rules alone");
	Check(CheatRuleFromFlags(0xC0) == CHEAT_RULE_SHARED, "3 is not a rule and reads as shared");
	Check(CheatRuleFromFlags(0) == CHEAT_RULE_SHARED,
	      "and a server too old to set the bits means shared");
}

void TestWhereATypedCheatRuns() {
	std::printf("\nwhere a typed cheat runs\n");

	CheatContext alone;
	bool vanilla = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		for (uint8_t rule = 0; rule <= CHEAT_RULE_OFF; ++rule) {
			alone.rule = rule;
			if (PlanTypedCheat(alone, id) != CheatVerdict::Vanilla)
				vanilla = false;
		}
	Check(vanilla, "with no session every cheat is single player's, whatever rule was last heard");

	CheatContext guest{true, false, CHEAT_RULE_SHARED};
	CheatContext host{true, true, CHEAT_RULE_SHARED};
	Check(PlanTypedCheat(guest, CHEAT_FOGGY) == CheatVerdict::SendToHost,
	      "a guest's PEASOUP goes to the host and is not run here");
	Check(PlanTypedCheat(host, CHEAT_FOGGY) == CheatVerdict::RunHereAndSend,
	      "the host's PEASOUP runs here, and the world packet goes at once");
	Check(PlanTypedCheat(guest, CHEAT_MAYHEM) == CheatVerdict::RunHereAndSend &&
	          PlanTypedCheat(host, CHEAT_SLOW_TIME) == CheatVerdict::RunHereAndSend,
	      "a riot or a slowed clock runs here and goes to everybody, host or not");
	Check(PlanTypedCheat(guest, CHEAT_WANTED_UP) == CheatVerdict::RunHere &&
	          PlanTypedCheat(guest, CHEAT_TANK) == CheatVerdict::RunHere &&
	          PlanTypedCheat(guest, CHEAT_BLOW_UP_CARS) == CheatVerdict::RunHere,
	      "MOREPOLICEPLEASE, the tank and BANGBANGBANG run here and nowhere else");

	CheatContext personal{true, false, CHEAT_RULE_PERSONAL};
	Check(PlanTypedCheat(personal, CHEAT_RAINY) == CheatVerdict::Refused &&
	          PlanTypedCheat(personal, CHEAT_BLOW_UP_CARS) == CheatVerdict::Refused,
	      "under personal the sky and BANGBANGBANG are refused");
	Check(PlanTypedCheat(personal, CHEAT_HEALTH) == CheatVerdict::RunHere,
	      "and GESUNDHEIT still works");
	CheatContext off{true, true, CHEAT_RULE_OFF};
	Check(PlanTypedCheat(off, CHEAT_MONEY) == CheatVerdict::Refused,
	      "under off not even the money");
	Check(std::strstr(CheatRefusalReason(CHEAT_RULE_OFF), "cheats = off") != nullptr &&
	          std::strstr(CheatRefusalReason(CHEAT_RULE_PERSONAL), "cheats = personal") != nullptr,
	      "and the log line names the setting that refused it");
}

// ---- the state a cheat leaves, and arriving at it --------------------------------

void TestTheTimeScaleAsAState() {
	std::printf("\nthe time scale as a state\n");
	uint8_t s = 0xFF;
	Check(TimeScaleToCheatState(1.0f, s) && s == CHEAT_TIME_SCALE_STATE_NORMAL, "1.0 is 2");
	Check(TimeScaleToCheatState(0.25f, s) && s == 0, "0.25 is 0");
	Check(TimeScaleToCheatState(4.0f, s) && s == 4, "4.0 is 4");
	Check(!TimeScaleToCheatState(0.3f, s) && !TimeScaleToCheatState(3.0f, s) &&
	          !TimeScaleToCheatState(8.0f, s),
	      "anything the two handlers cannot reach is not a state");
}

// The two handlers' own arithmetic, off their disassembly: double below 4,
// halve above 0.25.
float FastHandler(float scale) { return scale < 4.0f ? scale * 2.0f : scale; }
float SlowHandler(float scale) { return scale > 0.25f ? scale * 0.5f : scale; }

void TestEveryTimeScaleCanReachEveryOther() {
	std::printf("\narriving at somebody else's time scale\n");
	const float scales[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
	int wrong = 0;
	for (float from : scales)
		for (uint8_t want = 0; want <= CHEAT_TIME_SCALE_STATE_MAX; ++want) {
			EngineCheatState engine;
			engine.timeScale = from;
			const CheatSteps steps = PlanRoutedCheat(CHEAT_FAST_TIME, want, engine);
			if (steps.refused) {
				++wrong;
				continue;
			}
			float scale = from;
			for (uint8_t i = 0; i < steps.count; ++i)
				scale = steps.handler == CHEAT_FAST_TIME ? FastHandler(scale)
				                                         : SlowHandler(scale);
			uint8_t got = 0xFF;
			if (!TimeScaleToCheatState(scale, got) || got != want)
				++wrong;
		}
	Check(wrong == 0, "25 starts and targets, each reached by the engine's own handlers");

	EngineCheatState mission;
	mission.timeScale = 0.3f;
	Check(PlanRoutedCheat(CHEAT_SLOW_TIME, 1, mission).refused,
	      "a time scale no cheat made is not stepped from");
}

void TestTogglesArriveRatherThanFlip() {
	std::printf("\na toggle arrives, it does not flip\n");
	EngineCheatState armed;
	armed.givePedsWeapons = true;
	Check(PlanRoutedCheat(CHEAT_WEAPONS_FOR_ALL, 1, armed).count == 0,
	      "already armed and told armed: nothing runs");
	Check(PlanRoutedCheat(CHEAT_WEAPONS_FOR_ALL, 0, armed).count == 1,
	      "armed and told unarmed: the handler runs once");
	EngineCheatState calm;
	Check(PlanRoutedCheat(CHEAT_FAST_WEATHER, 1, calm).count == 1 &&
	          PlanRoutedCheat(CHEAT_FAST_WEATHER, 1, calm).handler == CHEAT_FAST_WEATHER,
	      "MADWEATHER off and told on: its own handler, once");
	Check(PlanRoutedCheat(CHEAT_MAYHEM, 1, calm).count == 1,
	      "a riot always runs - writing the table twice is the same table");
	Check(PlanRoutedCheat(CHEAT_CLOUDY, 0, calm).handler == CHEAT_CLOUDY,
	      "a sky runs its own ForceWeatherNow");
	Check(PlanRoutedCheat(CHEAT_MAYHEM, 0, calm).refused,
	      "an un-riot is refused: there is no such thing");
	Check(PlanRoutedCheat(CHEAT_MONEY, 0, calm).refused,
	      "and a personal cheat off the wire is refused outright");

	uint8_t state = 0xFF;
	Check(CurrentCheatState(CHEAT_WEAPONS_FOR_ALL, armed, state) && state == 1,
	      "what WEAPONSFORALL left us at is read back as 1");
}

// ---- the two exceptions -------------------------------------------------------------

void TestGesundheitLeavesSomebodyElsesCarAlone() {
	std::printf("\nGESUNDHEIT in the passenger seat\n");
	Check(HealthCheatMayRepairCar(false, true, false),
	      "single player heals the car whoever is driving");
	Check(HealthCheatMayRepairCar(true, true, true), "the driver heals his own car");
	Check(!HealthCheatMayRepairCar(true, true, false),
	      "a passenger does not heal somebody else's");
	Check(HealthCheatMayRepairCar(true, false, false), "on foot there is no car to argue about");
}

void TestReplicasDoNotLookForThreats() {
	std::printf("\nScanForThreats on a ped CoopIII built\n");
	Check(MayScanForThreats(false, false), "the engine's own pedestrians scan as always");
	Check(!MayScanForThreats(true, false), "a remote player does not");
	Check(!MayScanForThreats(false, true), "a replica pedestrian does not");
}

// ---- BANGBANGBANG ------------------------------------------------------------------

// The cheat calls slot 29 on every car in the pool, and the detour classifies
// each by what it is, never by who called. So the refusals are the same ones
// protocols 23, 24, 28 and 30 added, walked here for every kind of car a pool
// can hold.
void TestBangBangBangMeetsTheSameRefusals() {
	std::printf("\nBANGBANGBANG over a pool of every kind of car\n");
	struct Car {
		const char *what;
		bool        weDrive, remoteDriver, custodian, replica;
		bool        mayBlow;
	};
	const Car pool[] = {
	    {"the car we are driving", true, false, false, false, true},
	    {"a session car another player drives (23)", false, true, false, false, false},
	    {"a car another player is settling (24, 30)", false, false, true, false, false},
	    {"a replica of another machine's traffic (28)", false, false, false, true, false},
	    {"a parked car, a session car nobody holds, our own traffic", false, false, false, false, true},
	};
	for (const Car &c : pool) {
		const CarOwner owner = ClassifyCar(false, c.weDrive, c.remoteDriver,
		                                   c.custodian, c.replica);
		Check(MayBlowUpCar(owner) == c.mayBlow, c.what);
	}

	Check(CAutomobile__BlowUpCar != CVehicle__BlowUpCarBase &&
	          CBoat__BlowUpCar != CVehicle__BlowUpCarBase &&
	          CAutomobile__BlowUpCar != CBoat__BlowUpCar,
	      "slot 29 has two bodies worth hooking and one that is a bare ret");
}

void TestAPoolOfWrecksFitsTheQueue() {
	std::printf("\na whole pool of wrecks in one frame\n");

	WreckQueue<VEHICLE_POOL_SIZE> q;
	for (uint16_t i = 0; i < VEHICLE_POOL_SIZE; ++i) {
		UnownedBlast b{};
		b.key.kind = (i % 2) ? UNOWNED_PARKED : UNOWNED_SESSION;
		b.key.id   = static_cast<uint16_t>(i / 2);
		q.Push(b);
	}
	Check(q.Count() == size_t(VEHICLE_POOL_SIZE) && q.Dropped() == 0,
	      "110 wrecks in one frame, none dropped");

	UnownedBlast dup{};
	dup.key.kind = UNOWNED_PARKED;
	dup.key.id   = 3;
	Check(!q.Push(dup), "the same car twice is one report");

	// Client drains four a frame.
	std::vector<UnownedBlast> out;
	int                       frames = 0;
	UnownedBlast              batch[4];
	for (;;) {
		const uint8_t n = q.Drain(batch, 4);
		if (n == 0)
			break;
		out.insert(out.end(), batch, batch + n);
		++frames;
	}
	bool inOrder = out.size() == size_t(VEHICLE_POOL_SIZE);
	for (size_t i = 0; inOrder && i < out.size(); ++i)
		if (out[i].key.id != i / 2 ||
		    out[i].key.kind != ((i % 2) ? UNOWNED_PARKED : UNOWNED_SESSION))
			inOrder = false;
	Check(inOrder, "every one comes out, oldest first");
	Check(frames == 28, "over 28 frames of four");

	// What the old ring of eight did with the same frame.
	WreckQueue<8> old;
	for (uint16_t i = 0; i < VEHICLE_POOL_SIZE; ++i) {
		UnownedBlast b{};
		b.key.kind = UNOWNED_PARKED;
		b.key.id   = i;
		old.Push(b);
	}
	Check(old.Count() == 8 && old.Dropped() == 102,
	      "an eight-slot ring loses 102 of them - the bug this replaced");
}

// ---- Client ---------------------------------------------------------------------------

int g_wreckAsks = 0;

// The other end of the same frame: somebody across town typed BANGBANGBANG,
// and every parked car it wrecked arrives here as an instruction for a car
// this machine has not streamed in. Each one has to be held until it is.
void TestAFarAwayReceiverHoldsAWholePoolOfWrecks() {
	std::printf("\nsomebody else's BANGBANGBANG, from across town\n");
	WorldBridge b;
	b.WreckUnownedVehicle = [](const UnownedVehicleKey &) {
		++g_wreckAsks;
		return UnownedWreckOutcome::NotHere;
	};
	Client c;
	c.SetBridge(b);
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject = 0;
	w.playerId = 1;
	w.netId = 101;
	w.maxPlayers = MAX_PLAYERS;
	w.snapshotHz = SNAPSHOT_HZ;
	w.hour = 12;
	w.minute = 0;
	w.weather = 0;
	w.weatherOld = 0;
	w.hostPlayerId = 0;
	w.flags = 0;
	Message m;
	m.opcode  = S_Welcome::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(w));
	std::memcpy(m.data.data(), &w, sizeof(w));
	c.HandleMessage(m);

	for (uint16_t id = 0; id < VEHICLE_POOL_SIZE; ++id) {
		S_UnownedBlowUp blast{};
		InitHeader(blast, 4000);
		blast.where.rot        = {0.0f, 0.0f, 0.0f, 1.0f};
		blast.reporterPlayerId = 0;
		blast.key.kind         = UNOWNED_PARKED;
		blast.key.id           = id;
		Message bm;
		bm.opcode  = S_UnownedBlowUp::OPCODE;
		bm.channel = CH_EVENT;
		bm.data.resize(sizeof(blast));
		std::memcpy(bm.data.data(), &blast, sizeof(blast));
		c.HandleMessage(bm);
	}
	g_wreckAsks = 0;
	c.Tick();
	Check(g_wreckAsks == VEHICLE_POOL_SIZE,
	      "all 110 are held and asked about, where 32 used to be");
}

struct CheatRecord {
	int     sessionCalls = 0;
	bool    inSession    = false;
	bool    isHost       = false;
	uint8_t rule         = 0xFF;
	std::vector<CheatBody> applied;
};
CheatRecord g_cheatRec;

WorldBridge CheatBridge() {
	g_cheatRec = CheatRecord{};
	WorldBridge b;
	b.SetCheatSession = [](bool inSession, bool isHost, uint8_t rule) {
		++g_cheatRec.sessionCalls;
		g_cheatRec.inSession = inSession;
		g_cheatRec.isHost    = isHost;
		g_cheatRec.rule      = rule;
	};
	b.ApplyRoutedCheat = [](uint8_t cheat, uint8_t state) {
		CheatBody body{};
		body.cheat = cheat;
		body.state = state;
		g_cheatRec.applied.push_back(body);
	};
	return b;
}

template <class T>
Message WrapCheat(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId, uint8_t hostPlayerId, uint8_t cheatRule) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject       = 0;
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.minute       = 0;
	w.weather      = 0;
	w.weatherOld   = 0;
	w.hostPlayerId = hostPlayerId;
	w.flags        = FlagsWithCheatRule(0, cheatRule);
	return w;
}

S_Cheat Typed(uint8_t by, uint8_t id, uint8_t state) {
	S_Cheat c;
	InitHeader(c, 2000);
	c.playerId   = by;
	c.body.cheat = id;
	c.body.state = state;
	return c;
}

void TestTheWelcomeTellsTheSeam() {
	std::printf("\nthe welcome tells the seam\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(2, 0, CHEAT_RULE_PERSONAL)));
	Check(c.CheatRule() == CHEAT_RULE_PERSONAL, "the rule comes off the welcome's bits 6-7");
	Check(g_cheatRec.sessionCalls > 0 && g_cheatRec.inSession && !g_cheatRec.isHost &&
	          g_cheatRec.rule == CHEAT_RULE_PERSONAL,
	      "and the seam hears it before any keystroke can: in a session, not host, personal");

	Client h;
	h.SetBridge(CheatBridge());
	h.HandleMessage(WrapCheat(Welcome(0, 0, CHEAT_RULE_SHARED)));
	Check(g_cheatRec.isHost, "a player who is the host is told so");
}

void TestARoutedCheatIsBroughtAboutHere() {
	std::printf("\nsomebody else's cheat\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_SHARED)));

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_MAYHEM, 1)));
	Check(g_cheatRec.applied.size() == 1 && g_cheatRec.applied[0].cheat == CHEAT_MAYHEM,
	      "the host's riot is run here");

	c.HandleMessage(WrapCheat(Typed(1, CHEAT_WEAPONS_FOR_ALL, 1)));
	Check(g_cheatRec.applied.size() == 1, "our own cheat coming back is not run twice");

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FOGGY, 0)));
	Check(g_cheatRec.applied.size() == 1,
	      "a sky that reaches a guest is dropped - the host's world packet is the sky");

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FAST_TIME, 9)));
	Check(g_cheatRec.applied.size() == 1, "a state that is not one is dropped");

	c.HandleMessage(WrapCheat(Typed(INVALID_PLAYER, CHEAT_SLOW_TIME, 1)));
	Check(g_cheatRec.applied.size() == 2 && g_cheatRec.applied[1].state == 1,
	      "a joiner's replay, from nobody in particular, is run");
}

void TestTheHostTakesASkyAndSendsItAtOnce() {
	std::printf("\na sky cheat reaching the host\n");
	Client h;
	h.SetBridge(CheatBridge());
	h.HandleMessage(WrapCheat(Welcome(0, 0, CHEAT_RULE_SHARED)));
	Check(!h.WorldSendPending(), "nothing waiting to begin with");
	h.HandleMessage(WrapCheat(Typed(3, CHEAT_RAINY, 0)));
	Check(g_cheatRec.applied.size() == 1 && g_cheatRec.applied[0].cheat == CHEAT_RAINY,
	      "the host runs the guest's ILOVESCOTLAND");
	Check(h.WorldSendPending(), "and its world packet goes without waiting out the second");
}

void TestTheRuleIsKeptEvenIfTheServerDoesNot() {
	std::printf("\na server that relays what the rule refuses\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_PERSONAL)));
	c.HandleMessage(WrapCheat(Typed(0, CHEAT_MAYHEM, 1)));
	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FAST_TIME, 3)));
	Check(g_cheatRec.applied.empty(), "under personal no riot and no clock arrive here");

	Client o;
	o.SetBridge(CheatBridge());
	o.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_OFF)));
	o.HandleMessage(WrapCheat(Typed(0, CHEAT_WEAPONS_FOR_ALL, 1)));
	Check(g_cheatRec.applied.empty(), "under off nothing does");
}

// ---- the table against the real exe ----------------------------------------------------

// Opt-in: COOPIII_GTA3_EXE names a retail 1.0 gta3.exe, or reference/bin/
// gta3.exe is found from the working directory or four levels above it (the
// build output). Without one this says so and checks nothing.
bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

void TestTheTableAgainstTheImage() {
	std::printf("\nthe cheat table against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "table against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The prologue the seam checks before it hooks, and the shift: `mov
	// edx,12h` and the buffer twice.
	Check(std::memcmp(&img[CPad__AddToPCCheatString - IMAGE_BASE],
	                  CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE,
	                  sizeof(CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE)) == 0,
	      "the function opens with the prologue the seam looks for");
	Check(Byte(img, 0x00492457) == 0xBA && Dword(img, 0x00492458) == 0x12,
	      "the shift starts at index 18 (mov edx,12h)");
	Check(Dword(img, 0x00492462) == CPad__KeyBoardCheatString &&
	          Dword(img, 0x00492468) == CPad__KeyBoardCheatString + 1,
	      "and moves KeyBoardCheatString[i] to [i+1]");

	// Then every row, through the same decoder game/cheats.cpp runs over the
	// live process before it hooks anything.
	const ImageByteFn fromFile = [](const void *ctx, uint32_t va) -> uint8_t {
		return (*static_cast<const std::vector<uint8_t> *>(ctx))[va - IMAGE_BASE];
	};
	DecodedCheatRow rows[CHEAT_COUNT];
	uint8_t         bad = 0xFF;
	Check(DecodeCheatRows(fromFile, &img, rows, &bad),
	      "23 rows in the shape addresses.h gives them, then the epilogue");
	bool pushes = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (rows[id].length != CHEAT_SITES[id].length) {
			pushes = false;
			std::printf("    row %u (%s) pushes %u, the table says %u\n", id,
			            Forward(id).c_str(), rows[id].length, CHEAT_SITES[id].length);
		}
	Check(pushes, "every pushed length is the table's, BOOOOORING's 16 included");
	Check(CheatRowsMatchTable(fromFile, &img, &bad),
	      "and every string, its NUL and every handler are too");
	if (bad < CHEAT_COUNT)
		std::printf("    first bad row: %u (%s)\n", bad, Forward(bad).c_str());

	// Slot 29 of every vehicle-pool vtable.
	Check(Dword(img, 0x00600C1C + 0x74) == CAutomobile__BlowUpCar &&
	          Dword(img, 0x00600EA4 + 0x74) == CBoat__BlowUpCar,
	      "CAutomobile and CBoat reach the two hooked BlowUpCars");
	Check(Dword(img, CHeli__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CPlane__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CTrain__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CVehicle__vtable + 0x74) == CVehicle__BlowUpCarBase,
	      "the heli, the planes, the trains and the base reach the empty one");
	const uint8_t empty[] = {0x83, 0xEC, 0x08, 0x89, 0x4C, 0x24, 0x04,
	                         0x83, 0xC4, 0x08, 0xC2, 0x04, 0x00};
	Check(std::memcmp(&img[CVehicle__BlowUpCarBase - IMAGE_BASE], empty, sizeof(empty)) == 0,
	      "which is sub esp,8 / mov [esp+4],ecx / add esp,8 / ret 4 and nothing else");

	// The fear copy in CPed's constructor, and ScanForThreats reading it.
	Check(Dword(img, 0x004C4CDF) == CPedType__ms_apPedType &&
	          Dword(img, 0x004C4CE8) == offs::PED_FEAR_FLAGS,
	      "CPed's constructor copies ms_apPedType[type]->m_threats into +0x188");
	Check(Dword(img, CPed__ScanForThreats + 0x0B) == offs::PED_FEAR_FLAGS,
	      "and ScanForThreats reads +0x188, not the table");
}

} // namespace

int RunCheatTests() {
	TestTheTableIsWhatTheEngineCompares();
	TestEachCheatFiresOnItsLastKeyAndOnlyThen();
	TestNoCheatFiresAnotherOnTheWay();
	TestBoooooringOnlyWorksAsTheFirstThingTyped();
	TestTheBufferKeepsTheLastTwentyNewestFirst();
	TestWhatEachCheatTouches();
	TestTheRuleDecidesWhatIsAllowed();
	TestWhereATypedCheatRuns();
	TestTheTimeScaleAsAState();
	TestEveryTimeScaleCanReachEveryOther();
	TestTogglesArriveRatherThanFlip();
	TestGesundheitLeavesSomebodyElsesCarAlone();
	TestReplicasDoNotLookForThreats();
	TestBangBangBangMeetsTheSameRefusals();
	TestAPoolOfWrecksFitsTheQueue();
	TestAFarAwayReceiverHoldsAWholePoolOfWrecks();
	TestTheWelcomeTellsTheSeam();
	TestARoutedCheatIsBroughtAboutHere();
	TestTheHostTakesASkyAndSendsItAtOnce();
	TestTheRuleIsKeptEvenIfTheServerDoesNot();
	TestTheTableAgainstTheImage();
	return g_cheatFailures;
}
