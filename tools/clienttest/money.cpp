// Money in a session: who the engine half says an award belongs to, what
// MoneySync sends and writes for each rule, the $250 for a helicopter, and -
// when a copy of the retail exe is handed over - the award function and its
// two callers read back out of it.
//
// Nothing here burns a car. What runs is every decision either side of the
// engine: whether this machine may pay for a wreck at all, whom it pays, and
// what a shared wallet writes over the local cash when the total comes back.

#include "client.h"
#include "game/darkel.h"
#include "game/heli.h"
#include "game/money.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_moneyFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_moneyFailures;
}

// ---- the rules -----------------------------------------------------------------------

void TestWhoDecidesAWreck() {
	std::printf("\nwho decides a wreck\n");
	Check(WhoDecidesWreck(CarOwner::RemoteDriver, false, false, false) == WreckDecider::Elsewhere &&
	          WhoDecidesWreck(CarOwner::RemoteHost, false, false, false) == WreckDecider::Elsewhere &&
	          WhoDecidesWreck(CarOwner::RemoteCustodian, false, false, false) ==
	              WreckDecider::Elsewhere,
	      "a car another machine drives, hosts or settles is decided there");
	Check(WhoDecidesWreck(CarOwner::Local, true, false, false) == WreckDecider::OnlyHere,
	      "the car we drive is ours alone");
	Check(WhoDecidesWreck(CarOwner::Local, false, true, false) == WreckDecider::OnlyHere,
	      "so is the one we settle");
	Check(WhoDecidesWreck(CarOwner::Local, false, false, true) == WreckDecider::OnlyHere,
	      "and the traffic we host");
	Check(WhoDecidesWreck(CarOwner::Local, false, false, false) == WreckDecider::Everywhere,
	      "a parked car is decided by every machine that has it");
}

void TestWhereAnAwardGoes() {
	std::printf("\nwhere an explosion award goes\n");
	const AwardCulprit N = AwardCulprit::Nobody, U = AwardCulprit::Us,
	                   T = AwardCulprit::Them;
	bool elsewhere = true;
	for (AwardCulprit c : {N, U, T})
		for (bool keyed : {false, true})
			if (RouteExplosionAward(WreckDecider::Elsewhere, keyed, c) != AwardRoute::Drop)
				elsewhere = false;
	Check(elsewhere, "an observer pays nobody, whoever did it - the owner's game decides");

	Check(RouteExplosionAward(WreckDecider::OnlyHere, false, U) == AwardRoute::PayHere,
	      "our car, our fire: the engine pays us, as in single player");
	Check(RouteExplosionAward(WreckDecider::OnlyHere, false, N) == AwardRoute::PayHere,
	      "our car burning out by itself: us too - nobody else watched it");
	Check(RouteExplosionAward(WreckDecider::OnlyHere, false, T) == AwardRoute::Forward,
	      "our traffic, their rocket: forwarded to them, not kept");

	Check(RouteExplosionAward(WreckDecider::Everywhere, true, U) == AwardRoute::Forward,
	      "a parked car we lit goes through the server even to us, so it is paid once");
	Check(RouteExplosionAward(WreckDecider::Everywhere, true, T) == AwardRoute::Forward,
	      "one they lit goes to them, and their copy says the same under the same name");
	Check(RouteExplosionAward(WreckDecider::Everywhere, true, N) == AwardRoute::Drop,
	      "one nobody lit pays nobody: 'whoever watched' is everybody");
	Check(RouteExplosionAward(WreckDecider::Everywhere, false, U) == AwardRoute::PayHere,
	      "without a name, our own copy pays us");
	Check(RouteExplosionAward(WreckDecider::Everywhere, false, T) == AwardRoute::Drop,
	      "and theirs pays them - so ours doesn't");
	Check(RouteExplosionAward(WreckDecider::Everywhere, false, N) == AwardRoute::Drop,
	      "and nobody's pays nobody");
}

// A bomb somebody else set off goes off on the machine simulating the car,
// whose bomb timer asks only about its own player. With money on it asks
// about the bomber, and the award then goes where RouteExplosionAward sends
// any other: once, to him.
void TestABombPaysItsBomber() {
	std::printf("\nwhat a bomb pays, and to whom\n");
	Check(BombGateAsksBomber(true, MONEY_RULE_OWN, true, true) &&
	          BombGateAsksBomber(true, MONEY_RULE_SHARED, true, true),
	      "under own and shared the gate asks about another player who set it off");
	Check(!BombGateAsksBomber(true, MONEY_RULE_OFF, true, true),
	      "with money off it does not: the award would be paid to whoever sits here");
	Check(!BombGateAsksBomber(false, MONEY_RULE_OWN, true, true) &&
	          !BombGateAsksBomber(true, MONEY_RULE_OWN, false, true),
	      "nor out of a session, nor without the award's detour to route it");
	Check(!BombGateAsksBomber(true, MONEY_RULE_OWN, true, false),
	      "our own bomb, a pedestrian's blast or nobody's is the engine's own question");

	const AwardCulprit U = AwardCulprit::Us, T = AwardCulprit::Them;
	// Bob drives the car alice rigged, and alice presses the detonator.
	Check(RouteExplosionAward(WreckDecider::OnlyHere, false, T) == AwardRoute::Forward,
	      "bob's machine, which decides his car, forwards alice's pay to her");
	Check(RouteExplosionAward(WreckDecider::Elsewhere, false, U) == AwardRoute::Drop,
	      "and alice's own copy, which her engine also blew up, pays her nothing: bob's "
	      "machine decides it");
	// A parked car every machine has: each one asks, and the key makes it once.
	Check(RouteExplosionAward(WreckDecider::Everywhere, true, T) == AwardRoute::Forward &&
	          RouteExplosionAward(WreckDecider::Everywhere, true, U) == AwardRoute::Forward,
	      "a parked car's award goes through the server from every copy, alice's too, "
	      "under the car's name, and is delivered once");
	Check(RouteExplosionAward(WreckDecider::Everywhere, false, T) == AwardRoute::Drop &&
	          RouteExplosionAward(WreckDecider::Everywhere, false, U) == AwardRoute::PayHere,
	      "a car no name reaches is paid by alice's copy alone");
}

void TestTheEnginesArithmetic() {
	std::printf("\nAwardMoneyForExplosion's arithmetic\n");
	Check(ExplosionAwardUnit(25000) == 50 && ExplosionAwardUnit(9000) == 18,
	      "nMonetaryValue * 0.002f: 25000 is $50, 9000 is $18");
	Check(ExplosionAwardUnit(499) == 0 && ExplosionAwardUnit(0) == 0,
	      "truncated, not rounded: $499 of car is worth nothing");

	ExplosionChain c;
	c.lastMs = 0;
	c.count  = 0;
	Check(ChainExplosionAward(c, 100000, 50) == 50 && c.count == 1,
	      "the first car on its own is one car");
	Check(ChainExplosionAward(c, 100000 + 5999, 50) == 100 && c.count == 2,
	      "another inside six seconds is the second link and pays double");
	Check(ChainExplosionAward(c, 100000 + 5999 + 3000, 40) == 120 && c.count == 3,
	      "the third pays three times its own car");
	Check(ChainExplosionAward(c, 100000 + 8999 + 6000, 50) == 50 && c.count == 1,
	      "six seconds exactly is too late (the jae is unsigned and inclusive)");
	c.lastMs = 200000;
	Check(ChainExplosionAward(c, 1000, 50) == 50 && c.count == 1,
	      "a clock that went backwards - a new game - starts a new chain");
}

void TestThePoolArithmetic() {
	std::printf("\nthe pool's arithmetic\n");
	Check(AddToMoneyPool(100, -250) == 0, "a fine bigger than the pool leaves it at zero");
	Check(AddToMoneyPool(INT32_MAX - 5, 10) == INT32_MAX, "and it stops at the top");
	Check(AddToMoneyPool(1000, -1000) == 0 && AddToMoneyPool(0, 250) == 250,
	      "everything else is addition");
	Check(IsSaneMoneyAward(1) && IsSaneMoneyAward(MONEY_AWARD_MAX_UNIT) &&
	          !IsSaneMoneyAward(0) && !IsSaneMoneyAward(-5) &&
	          !IsSaneMoneyAward(MONEY_AWARD_MAX_UNIT + 1),
	      "an award is a positive number no car exceeds");
	UnownedVehicleKey k{};
	k.kind = UNOWNED_PARKED;
	const bool parked = MoneyAwardKeyed(k);
	k.kind = UNOWNED_SESSION;
	const bool session = MoneyAwardKeyed(k);
	k.kind = UNOWNED_AMBIENT;
	const bool ambient = MoneyAwardKeyed(k);
	k.kind = MONEY_AWARD_UNKEYED;
	Check(parked && session && !ambient && !MoneyAwardKeyed(k),
	      "only the two names several machines share are keys");
}

// ---- MoneySync -------------------------------------------------------------------------

struct Rec {
	bool     haveSession  = false;
	bool     inSession    = false;
	uint8_t  rule         = 0xEE;
	bool     hasPlayer    = true;
	int32_t  cash         = 0;
	int32_t  life         = 1;
	int      writes       = 0;
	std::vector<LocalMoneyAward> queued;
	std::vector<int32_t>         paid;
	std::vector<Message>         sent;
};
Rec g_rec;

void RecSession(bool inSession, uint8_t rule) {
	g_rec.haveSession = true;
	g_rec.inSession   = inSession;
	g_rec.rule        = rule;
}
bool RecRead(int32_t &money, int32_t &life) {
	if (!g_rec.hasPlayer)
		return false;
	money = g_rec.cash;
	life  = g_rec.life;
	return true;
}
void RecWrite(int32_t money) {
	++g_rec.writes;
	g_rec.cash = money;
}
uint8_t RecDrain(LocalMoneyAward *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && !g_rec.queued.empty()) {
		out[n++] = g_rec.queued.front();
		g_rec.queued.erase(g_rec.queued.begin());
	}
	return n;
}
void RecPay(int32_t unit) { g_rec.paid.push_back(unit); }
void RecSend(void *, const void *bytes, size_t len, Channel ch) {
	Message m;
	m.opcode  = static_cast<const uint8_t *>(bytes)[0];
	m.channel = ch;
	m.data.assign(static_cast<const uint8_t *>(bytes),
	              static_cast<const uint8_t *>(bytes) + len);
	g_rec.sent.push_back(m);
}
// Player 2 is netId 202 and nobody else is anybody.
uint8_t RecRoster(void *, uint16_t netId) { return netId == 202 ? 2 : INVALID_PLAYER; }

MoneyBridge RecBridge() {
	MoneyBridge b;
	b.SetMoneySession   = &RecSession;
	b.ReadMoney         = &RecRead;
	b.WriteMoney        = &RecWrite;
	b.DrainMoneyAwards  = &RecDrain;
	b.PayExplosionAward = &RecPay;
	return b;
}

MoneySync &FreshMoney() {
	static MoneyBridge bridge;
	static MoneySync   sync;
	g_rec  = Rec{};
	bridge = RecBridge();
	sync   = MoneySync{};
	sync.Bind(&bridge, &RecSend, nullptr, &RecRoster, nullptr);
	return sync;
}

template <class T>
std::vector<T> SentOf() {
	std::vector<T> out;
	for (const Message &m : g_rec.sent)
		if (const T *p = m.as<T>())
			out.push_back(*p);
	return out;
}

S_Money Pool(uint8_t rule, bool seeded, int32_t total, uint32_t ack,
             uint8_t from = INVALID_PLAYER, int32_t delta = 0) {
	S_Money m;
	InitHeader(m, 1000);
	m.rule         = rule;
	m.flags        = seeded ? uint8_t(MONEY_POOL_SEEDED) : uint8_t(0);
	m.fromPlayerId = from;
	m.pad          = 0;
	m.total        = total;
	m.ackSeq       = ack;
	m.delta        = delta;
	return m;
}

S_MoneyAward AwardFor(uint8_t to, int32_t unit, uint8_t from = 0) {
	S_MoneyAward a;
	InitHeader(a, 1000);
	a.fromPlayerId    = from;
	a.body.toPlayerId = to;
	a.body.kind       = MONEY_AWARD_FIRE;
	a.body.model      = 90;
	a.body.unit       = unit;
	a.body.key        = UnownedVehicleKey{MONEY_AWARD_UNKEYED, 0, 0};
	return a;
}

LocalMoneyAward Forwarded(bool toUs, uint16_t toNet, uint8_t toId, int32_t unit) {
	LocalMoneyAward a;
	a.toUs       = toUs;
	a.toNetId    = toNet;
	a.toPlayerId = toId;
	a.unit       = unit;
	a.model      = 91;
	return a;
}

void TestOffIsNothing() {
	std::printf("\nmoney off: nothing new\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 1000;
	g_rec.queued.push_back(Forwarded(true, INVALID_NETID, INVALID_PLAYER, 50));
	m.Send(1, 2000);
	g_rec.cash = 1100;
	m.Send(1, 2040);
	Check(g_rec.sent.empty(), "with no S_Money nothing goes out, whatever the cash does");
	Check(g_rec.queued.empty(), "and an award the seam queued anyway is thrown away");
	m.OnAward(AwardFor(1, 50), 1);
	Check(g_rec.paid.empty(), "an award off the wire is not paid");
	Check(!g_rec.haveSession, "and the seam is never told there is a rule");

	m.OnMoney(Pool(MONEY_RULE_OFF, false, 0, 0), 1);
	Check(m.Rule() == MONEY_RULE_OFF && !g_rec.haveSession && g_rec.writes == 0,
	      "an S_Money that says off changes nothing either");
}

void TestOwnForwardsAwardsAndKeepsWallets() {
	std::printf("\nmoney own\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 1000;
	m.OnMoney(Pool(MONEY_RULE_OWN, false, 0, 0), 1);
	Check(m.Rule() == MONEY_RULE_OWN && g_rec.inSession && g_rec.rule == MONEY_RULE_OWN,
	      "the seam hears the rule at once");

	g_rec.queued.push_back(Forwarded(false, 202, INVALID_PLAYER, 50));   // their ped
	g_rec.queued.push_back(Forwarded(false, INVALID_NETID, 3, 18));      // their car
	LocalMoneyAward keyed = Forwarded(true, INVALID_NETID, INVALID_PLAYER, 40);
	keyed.key.kind        = UNOWNED_PARKED;
	keyed.key.id          = 17;
	g_rec.queued.push_back(keyed);                                        // our parked car
	g_rec.queued.push_back(Forwarded(false, 999, INVALID_PLAYER, 50));   // somebody gone
	g_rec.cash = 1250;
	m.Send(1, 2000);

	const std::vector<C_MoneyAward> out = SentOf<C_MoneyAward>();
	Check(out.size() == 3, "three awards out, the one for a player who left dropped");
	Check(out.size() == 3 && out[0].body.toPlayerId == 2 && out[0].body.unit == 50,
	      "the one the engine named by ped goes to that ped's player");
	Check(out.size() == 3 && out[1].body.toPlayerId == 3 && out[1].body.unit == 18,
	      "the one it named by car goes to the driver");
	Check(out.size() == 3 && out[2].body.toPlayerId == 1 &&
	          out[2].body.key.kind == UNOWNED_PARKED && out[2].body.key.id == 17,
	      "and ours on a parked car goes to us, through the server, under its name");
	Check(SentOf<C_MoneyChange>().empty() && g_rec.writes == 0,
	      "own wallets: the cash moving here is nobody's business");

	m.OnAward(AwardFor(1, 50, 0), 1);
	Check(g_rec.paid.size() == 1 && g_rec.paid[0] == 50, "an award for us is paid");
	m.OnAward(AwardFor(2, 50, 0), 1);
	Check(g_rec.paid.size() == 1, "one for somebody else is not");
	m.OnAward(AwardFor(1, MONEY_AWARD_MAX_UNIT + 1, 0), 1);
	m.OnAward(AwardFor(1, 0, 0), 1);
	Check(g_rec.paid.size() == 1, "nor is a nonsense amount");

	m.Clear();
	Check(m.Rule() == MONEY_RULE_OFF && !g_rec.inSession && g_rec.rule == MONEY_RULE_OFF,
	      "a lost session puts the seam back to off");
	m.OnAward(AwardFor(1, 50, 0), 1);
	Check(g_rec.paid.size() == 1, "after which nothing off the wire is paid");
	Check(g_rec.cash == 1250 && g_rec.writes == 0, "and the cash is left where it was");
}

void TestTheFirstPlayerSeedsTheSharedWallet() {
	std::printf("\nmoney shared: an empty pool\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 5000;
	m.OnMoney(Pool(MONEY_RULE_SHARED, false, 0, 0), 1);
	const std::vector<C_MoneyChange> seed = SentOf<C_MoneyChange>();
	Check(seed.size() == 1 && seed[0].body.seq == 1 && seed[0].body.delta == 0 &&
	          seed[0].body.have == 5000,
	      "our $5000 goes out as the seed");
	Check(g_rec.writes == 0, "and nothing is written until the pool comes back");

	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 5000, 1, 1, 0), 1);
	Check(g_rec.writes == 0 && m.PendingCount() == 0,
	      "the pool is our own money, so there is nothing to write");

	// The same thing with no player yet: the seed waits for one.
	MoneySync &n = FreshMoney();
	g_rec.hasPlayer = false;
	n.OnMoney(Pool(MONEY_RULE_SHARED, false, 0, 0), 1);
	Check(SentOf<C_MoneyChange>().empty(), "no player, no seed");
	g_rec.hasPlayer = true;
	g_rec.cash      = 800;
	n.Send(1, 2000);
	const std::vector<C_MoneyChange> late = SentOf<C_MoneyChange>();
	Check(late.size() == 1 && late[0].body.have == 800 && late[0].body.delta == 0,
	      "the first frame with one sends it");
}

void TestTheSharedWalletFollowsTheServer() {
	std::printf("\nmoney shared: changes both ways\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 300;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	Check(g_rec.writes == 1 && g_rec.cash == 1000,
	      "a joiner's cash is the session's (m_nMoney only; the HUD walks to it)");
	m.Send(1, 2000);
	Check(SentOf<C_MoneyChange>().empty(), "and writing it is not a change of ours");

	g_rec.cash = 900;   // a Pay'n'Spray
	m.Send(1, 2040);
	std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == -100 && sent[0].body.have == 900,
	      "spending $100 here goes out as -100");
	m.Send(1, 2080);
	Check(SentOf<C_MoneyChange>().size() == 1, "once");
	const uint32_t spend = sent.empty() ? 0 : sent[0].body.seq;

	// Before the server has it, somebody else earns $500.
	const int writesBefore = g_rec.writes;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1500, spend - 1, 2, 500), 1);
	Check(g_rec.cash == 1400,
	      "their $500 arrives on a total without our $100: we get 1400, not 1500");
	Check(g_rec.writes == writesBefore + 1 && m.PendingCount() == 1,
	      "one write, and our change still counted as on its way");

	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1400, spend, 1, -100), 1);
	Check(g_rec.cash == 1400 && g_rec.writes == writesBefore + 1 && m.PendingCount() == 0,
	      "the server's echo of our own change moves nothing - no roll back and forth");

	// A frame that earns and a packet that lands before PostFrame could send it.
	g_rec.cash = 1650;   // a $250 helicopter, paid inside CGame::Process
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1420, spend, 2, 20), 1);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 2 && sent[1].body.delta == 250,
	      "our unsent $250 goes out before the total is written");
	Check(g_rec.cash == 1670, "and is kept on top of their $20: 1420 + 250");

	g_rec.cash = 1570;   // busted with one star
	m.Send(1, 3000);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 3 && sent[2].body.delta == -100, "a fine is a change like any other");
}

void TestANewPlayerIsNotAChange() {
	std::printf("\nmoney shared: a load or a new game\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 2000;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 2000, 0), 1);
	g_rec.life = 2;
	g_rec.cash = 45000;   // the save's money
	m.Send(1, 2000);
	Check(SentOf<C_MoneyChange>().empty(), "a rebuilt player's money is not sent as a change");
	Check(g_rec.cash == 2000, "the session's is written over it");

	g_rec.hasPlayer = false;   // the menus
	m.Send(1, 2040);
	g_rec.hasPlayer = true;
	g_rec.cash      = 0;       // CPlayerInfo::Clear on the way into a new game
	m.Send(1, 2080);
	Check(SentOf<C_MoneyChange>().empty() && g_rec.cash == 2000,
	      "nor is anything seen after a gap with no player");

	g_rec.cash = 2100;
	m.Send(1, 2120);
	const std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 100, "the next real change is");
}

S_RampageEnd Passed(uint8_t payer) {
	S_RampageEnd end;
	InitHeader(end, 0);
	end.body.frenzyId = 3;
	end.body.outcome  = RAMPAGE_PASSED;
	end.payerId       = payer;
	return end;
}

// rampage.sc pays its own player on every machine in the frenzy; one wallet
// takes one of them.
void TestASharedRampagePaysTheWalletOnce() {
	std::printf("\nmoney shared: a rampage every machine's script pays for\n");
	Check(IsRampageReward(5000) && IsRampageReward(45000) && IsRampageReward(95000) &&
	          IsRampageReward(1000000),
	      "what rampage.sc pays: $5000 a rampage passed, or a million for the last");
	Check(!IsRampageReward(100000) && !IsRampageReward(250) && !IsRampageReward(5250) &&
	          !IsRampageReward(0) && !IsRampageReward(-5000),
	      "and nothing else looks like it");

	MoneySync &m = FreshMoney();
	g_rec.cash   = 1000;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	m.Send(1, 2000);

	m.OnRampageEnd(Passed(0), 1, 2000);
	Check(m.WaitingForRampageReward(), "player 0's report won, so ours is the one to give back");
	const int writes = g_rec.writes;
	g_rec.cash       = 1000 + 15000;   // our third rampage
	m.Send(1, 2016);
	Check(SentOf<C_MoneyChange>().empty(), "our script's $15000 doesn't go into the wallet");
	Check(g_rec.cash == 1000 && g_rec.writes == writes + 1, "and is taken off our cash");
	Check(!m.WaitingForRampageReward(), "once");

	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 6000, 0, 0, 5000), 1);
	Check(g_rec.cash == 6000, "the payer's $5000 reaches us through the wallet like any change");
	g_rec.cash = 21000;   // a second $15000 after that is not a rampage's
	m.Send(1, 2100);
	std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 15000, "the next one is ours to send");

	// The payer's own machine keeps its reward, and sends it.
	MoneySync &p = FreshMoney();
	g_rec.cash   = 1000;
	p.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	p.Send(1, 2000);
	p.OnRampageEnd(Passed(1), 1, 2000);
	g_rec.cash = 6000;
	p.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(!p.WaitingForRampageReward() && sent.size() == 1 && sent[0].body.delta == 5000,
	      "the payer's machine sends its reward as a change");

	// A change in the window that isn't the reward goes out; the wait runs out.
	MoneySync &w = FreshMoney();
	g_rec.cash   = 1000;
	w.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	w.Send(1, 2000);
	w.OnRampageEnd(Passed(0), 1, 2000);
	g_rec.cash = 900;
	w.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == -100 && w.WaitingForRampageReward(),
	      "a fine in the meantime still goes out, and the wait goes on");
	w.Send(1, 2000 + RAMPAGE_REWARD_WAIT_MS + 1);
	Check(!w.WaitingForRampageReward(), "until it runs out");

	// Under own each wallet keeps its own; an old server names no payer.
	MoneySync &o = FreshMoney();
	o.OnMoney(Pool(MONEY_RULE_OWN, false, 0, 0), 1);
	o.OnRampageEnd(Passed(0), 1, 2000);
	Check(!o.WaitingForRampageReward(), "under own nobody's reward is taken back");
	MoneySync &n = FreshMoney();
	g_rec.cash   = 1000;
	n.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	n.OnRampageEnd(Passed(INVALID_PLAYER), 1, 2000);
	S_RampageEnd failed = Passed(0);
	failed.body.outcome = RAMPAGE_FAILED;
	n.OnRampageEnd(failed, 1, 2000);
	Check(!n.WaitingForRampageReward(), "nor with no payer, nor for a failure");
}

// The engine half measures what our rampage.sc paid over the script's own
// pass, so a gain in the same frame is no longer mistaken for part of it.
void TestARampageRewardInABusyFrame() {
	std::printf("\nmoney shared: a rampage's reward in the same frame as other money\n");

	// Before: with only the size to go on, $15000 + a $250 helicopter is not
	// a reward, and the whole $15250 went into the wallet.
	MoneySync &s = FreshMoney();
	g_rec.cash   = 1000;
	s.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	s.Send(1, 2000);
	s.OnRampageEnd(Passed(0), 1, 2000);
	g_rec.cash = 1000 + 15000 + 250;
	s.Send(1, 2016);
	std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 15250 && s.WaitingForRampageReward(),
	      "without the engine's measurement the reward goes in with the $250");

	// Now: the engine half says what the script paid, and that is what goes.
	MoneySync &m = FreshMoney();
	g_rec.cash   = 1000;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	m.Send(1, 2000);
	m.OnRampageEnd(Passed(0), 1, 2000);
	m.OnRampageRewardPaid(15000);
	g_rec.cash = 1000 + 15000 + 250;
	m.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 250 && sent[0].body.have == 1250,
	      "the $250 goes into the wallet on its own");
	Check(g_rec.cash == 1250 && !m.WaitingForRampageReward(),
	      "and exactly the $15000 is taken off our cash, once");
	g_rec.cash = 16250;
	m.Send(1, 2100);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 2 && sent[1].body.delta == 15000, "a later $15000 is ours to send");

	// A frame that also spent money: the loss goes out, the reward doesn't.
	MoneySync &l = FreshMoney();
	g_rec.cash   = 1000;
	l.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	l.Send(1, 2000);
	l.OnRampageEnd(Passed(0), 1, 2000);
	l.OnRampageRewardPaid(5000);
	g_rec.cash = 1000 + 5000 - 100;
	l.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == -100 && g_rec.cash == 900,
	      "a fine in the same frame goes out, and the reward is still taken back");

	// Exactly the reward and nothing else: nothing sent, as before.
	MoneySync &e = FreshMoney();
	g_rec.cash   = 1000;
	e.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	e.Send(1, 2000);
	e.OnRampageEnd(Passed(0), 1, 2000);
	e.OnRampageRewardPaid(5000);
	g_rec.cash = 6000;
	e.Send(1, 2016);
	Check(SentOf<C_MoneyChange>().empty() && g_rec.cash == 1000,
	      "the reward alone: taken back, nothing sent");

	// Not owed: the payer's own machine, or a measurement that isn't a reward.
	MoneySync &p = FreshMoney();
	g_rec.cash   = 1000;
	p.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	p.Send(1, 2000);
	p.OnRampageEnd(Passed(1), 1, 2000);
	p.OnRampageRewardPaid(5000);
	g_rec.cash = 6250;
	p.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 5250,
	      "the payer's machine sends all of it, reward and all");
	MoneySync &o = FreshMoney();
	g_rec.cash   = 1000;
	o.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	o.Send(1, 2000);
	o.OnRampageEnd(Passed(0), 1, 2000);
	o.OnRampageRewardPaid(5250);
	g_rec.cash = 6250;
	o.Send(1, 2016);
	sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 5250 && o.WaitingForRampageReward(),
	      "and a measured amount rampage.sc can't pay is not taken for one");
}

void TestAPoolThatEmptiesIsSeededAgain() {
	std::printf("\nmoney shared: the rule changes under us\n");
	MoneySync &m = FreshMoney();
	g_rec.cash   = 700;
	m.OnMoney(Pool(MONEY_RULE_OWN, false, 0, 0), 1);
	m.OnMoney(Pool(MONEY_RULE_SHARED, false, 0, 0), 1);
	const std::vector<C_MoneyChange> seed = SentOf<C_MoneyChange>();
	Check(seed.size() == 1 && seed[0].body.have == 700, "own to shared seeds with what we have");
	m.OnMoney(Pool(MONEY_RULE_OFF, false, 0, 0), 1);
	g_rec.cash = 900;
	m.Send(1, 2000);
	Check(SentOf<C_MoneyChange>().size() == 1 && g_rec.rule == MONEY_RULE_OFF,
	      "and back to off stops every change");
}

// Under `shared` our own cash is kept beside the wallet: it is what a save
// made in the session stores and what leaving puts back in our pocket.
struct OwnRec {
	int     pushes = 0;
	bool    have   = false;
	int32_t own    = 0;
	int32_t credit = 0;   // what the engine half kept out of m_nMoney for us
};
OwnRec g_own;
int32_t RecCredit() {
	const int32_t c = g_own.credit;
	g_own.credit    = 0;
	return c;
}
void RecOwn(bool have, int32_t own) {
	++g_own.pushes;
	g_own.have = have;
	g_own.own  = own;
}

MoneySync &FreshMoneyWithOwn() {
	static MoneyBridge bridge;
	static MoneySync   sync;
	g_rec           = Rec{};
	g_own           = OwnRec{};
	bridge          = RecBridge();
	bridge.SetOwnMoney = &RecOwn;
	bridge.DrainOwnCredit = &RecCredit;
	sync            = MoneySync{};
	sync.Bind(&bridge, &RecSend, nullptr, &RecRoster, nullptr);
	return sync;
}

// A hidden package somebody found under `shared`: his engine pays him, which
// is one change to the wallet; every other machine counts it (pickup.cpp,
// CountPackageForUs) and puts its $1000 on its own cash alone.
void TestAGroupPackagePaysTheWalletOnce() {
	std::printf("\nmoney shared: a hidden package pays the wallet once and every save\n");
	Check(PackageMoneyIsOwnOnly(true, MONEY_RULE_SHARED) &&
	          !PackageMoneyIsOwnOnly(true, MONEY_RULE_OWN) &&
	          !PackageMoneyIsOwnOnly(true, MONEY_RULE_OFF) &&
	          !PackageMoneyIsOwnOnly(false, MONEY_RULE_SHARED),
	      "only under shared is an observer's package money kept out of the cash");

	// The collector: his engine's COLLECTABLE1 arm adds $1000 to his cash.
	MoneySync &a = FreshMoneyWithOwn();
	g_rec.cash   = 300;
	a.OnMoney(Pool(MONEY_RULE_SHARED, true, 2000, 0), 1);
	g_rec.cash = 3000;
	a.Send(1, 2000);
	std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	Check(sent.size() == 1 && sent[0].body.delta == 1000,
	      "the collector's $1000 goes into the wallet, as his change");
	Check(a.Own() == 1300, "and onto his own cash");

	// Everybody else: the engine half kept it out of the cash.
	MoneySync &b = FreshMoneyWithOwn();
	g_rec.cash   = 500;
	b.OnMoney(Pool(MONEY_RULE_SHARED, true, 2000, 0), 2);
	g_own.credit = 1000;
	b.Send(2, 2000);
	Check(SentOf<C_MoneyChange>().empty() && g_rec.cash == 2000,
	      "an observer sends nothing for it: the wallet is not paid twice");
	Check(b.Own() == 1500 && g_own.own == 1500,
	      "but it is on his own cash, which is what his save holds");
	b.OnMoney(Pool(MONEY_RULE_SHARED, true, 3000, 0, 1, 1000), 2);
	Check(g_rec.cash == 3000 && b.Own() == 1500,
	      "the collector's $1000 reaches him through the wallet, and only there");
	g_own.credit = 1001000;   // the hundredth
	b.Send(2, 2040);
	Check(SentOf<C_MoneyChange>().empty() && b.Own() == 1002500,
	      "the million at the hundredth likewise");
	b.Clear();
	Check(g_rec.cash == 1002500, "and leaving hands him all of it");

	// Counted before there was a player to read, as a joiner's batch can be.
	MoneySync &c = FreshMoneyWithOwn();
	g_rec.hasPlayer = false;
	c.OnMoney(Pool(MONEY_RULE_SHARED, true, 2000, 0), 3);
	g_own.credit = 3000;
	c.Send(3, 2000);
	Check(c.OwnCreditWaiting() == 3000, "it waits for an own cash to go onto");
	g_rec.hasPlayer = true;
	g_rec.cash      = 100;
	c.Send(3, 2040);
	Check(c.Own() == 3100 && c.OwnCreditWaiting() == 0 && g_rec.cash == 2000 &&
	          SentOf<C_MoneyChange>().empty(),
	      "and goes onto it, not into the wallet, when the player is read");

	// Leaving before the wallet ever went over the cash: it goes onto the cash.
	MoneySync &d = FreshMoneyWithOwn();
	g_rec.hasPlayer = false;
	g_rec.cash      = 700;
	d.OnMoney(Pool(MONEY_RULE_SHARED, true, 2000, 0), 3);
	g_own.credit    = 1000;
	g_rec.hasPlayer = true;
	d.Clear();
	Check(g_rec.cash == 1700, "leaving before any read puts it on the cash, which is ours");
}

void TestOurOwnCashBesideTheWallet() {
	std::printf("\nmoney shared: our own cash, for the save and for leaving\n");
	Check(OwnMoneyAfter(300, -1000) == 0 && OwnMoneyAfter(300, 200) == 500,
	      "our own cash moves like the wallet and never below nothing");
	Check(OwnMoneyOnLeaving(450, 1650, 1700) == 400 &&
	          OwnMoneyOnLeaving(450, INT32_MIN, INT32_MAX) == 0,
	      "leaving counts the change nobody has read yet, without overflowing");
	Check(SaveWritesOwnMoney(true, MONEY_RULE_SHARED, true) &&
	          !SaveWritesOwnMoney(true, MONEY_RULE_OWN, true) &&
	          !SaveWritesOwnMoney(true, MONEY_RULE_OFF, true) &&
	          !SaveWritesOwnMoney(false, MONEY_RULE_SHARED, true) &&
	          !SaveWritesOwnMoney(true, MONEY_RULE_SHARED, false),
	      "a save swaps the cash only in a session, under shared, with our own known");

	MoneySync &m = FreshMoneyWithOwn();
	g_rec.cash   = 300;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1000, 0), 1);
	Check(g_rec.cash == 1000 && m.HaveOwn() && m.Own() == 300,
	      "joining a wallet of $1000 with $300: the wallet in our pocket, $300 our own");
	Check(g_own.have && g_own.own == 300, "and the engine half is told, for the save");

	g_rec.cash = 900;   // a Pay'n'Spray here
	m.Send(1, 2000);
	Check(m.Own() == 200 && g_own.own == 200, "what we spend comes off our own");
	const std::vector<C_MoneyChange> sent = SentOf<C_MoneyChange>();
	const uint32_t spend = sent.empty() ? 0 : sent.back().body.seq;
	m.OnMoney(Pool(MONEY_RULE_SHARED, true, 1400, spend, 2, 500), 1);
	Check(g_rec.cash == 1400 && m.Own() == 200,
	      "a teammate's $500 goes into the wallet and not into our own");
	g_rec.cash = 1650;   // a helicopter here
	m.Send(1, 2040);
	Check(m.Own() == 450, "what we earn goes onto our own");
	g_rec.cash = 1650 + 1000000;
	m.Send(1, 2080);
	g_rec.cash = 1650;
	m.Send(1, 2120);
	Check(m.Own() == 450, "up and back down again is nothing");

	g_rec.cash = 1600;   // spent in the frame the connection went, never read
	const int writes = g_rec.writes;
	m.Clear();
	Check(g_rec.cash == 400 && g_rec.writes == writes + 1,
	      "leaving the session puts our own back: $450, less the $50 nobody read");
	Check(!g_own.have && !m.HaveOwn(), "and there is no own cash to save any more");
	m.Clear();
	Check(g_rec.cash == 400 && g_rec.writes == writes + 1, "once");

	// Spending more than was ours leaves us with nothing, not a debt.
	MoneySync &d = FreshMoneyWithOwn();
	g_rec.cash   = 300;
	d.OnMoney(Pool(MONEY_RULE_SHARED, true, 5000, 0), 1);
	g_rec.cash = 1000;
	d.Send(1, 2000);
	d.Clear();
	Check(g_rec.cash == 0, "$4000 of the wallet spent on $300 of our own leaves $0");

	// The rule going to own mid-session is leaving the wallet too.
	MoneySync &r = FreshMoneyWithOwn();
	g_rec.cash   = 700;
	r.OnMoney(Pool(MONEY_RULE_SHARED, true, 9000, 0), 1);
	r.OnMoney(Pool(MONEY_RULE_OWN, false, 0, 0), 1);
	Check(g_rec.cash == 700 && r.Rule() == MONEY_RULE_OWN && !r.HaveOwn(),
	      "shared to own puts our $700 back in place of the $9000");

	// A load in the session: that save's money is our own from then on.
	MoneySync &l = FreshMoneyWithOwn();
	g_rec.cash   = 2000;
	l.OnMoney(Pool(MONEY_RULE_SHARED, true, 8000, 0), 1);
	g_rec.hasPlayer = false;
	l.Send(1, 2000);
	g_rec.hasPlayer = true;
	g_rec.life      = 2;
	g_rec.cash      = 45000;   // the save's
	l.Send(1, 2040);
	Check(g_rec.cash == 8000 && l.Own() == 45000,
	      "a save loaded in the session: the wallet in our pocket, the save's $45000 our own");
	l.Clear();
	Check(g_rec.cash == 45000, "and that is what leaving gives back");

	// Leaving while there is no player to read: nothing is written.
	MoneySync &g = FreshMoneyWithOwn();
	g_rec.cash   = 100;
	g.OnMoney(Pool(MONEY_RULE_SHARED, true, 600, 0), 1);
	g_rec.hasPlayer = false;
	const int before = g_rec.writes;
	g.Clear();
	Check(g_rec.writes == before, "no player to put it back into, nothing is written");

	// Under own the cash was always ours, and nothing is kept beside it.
	MoneySync &o = FreshMoneyWithOwn();
	g_rec.cash   = 1250;
	o.OnMoney(Pool(MONEY_RULE_OWN, false, 0, 0), 1);
	g_rec.cash = 1300;
	o.Send(1, 2000);
	o.Clear();
	Check(!g_own.have && g_rec.cash == 1300 && g_rec.writes == 0,
	      "under own there is no own cash to keep, and leaving writes nothing");
}

// ---- Client and the helicopter -------------------------------------------------------

struct HeliPayRec {
	int credits    = 0;
	int crimesOnly = 0;
	int paid       = 0;
};
HeliPayRec g_heliPay;
void HeliCredit(uint8_t, const Vec3 &, bool statistics) {
	++g_heliPay.credits;
	if (!statistics)
		++g_heliPay.crimesOnly;
}
void HeliPay() { ++g_heliPay.paid; }

template <class T>
Message Wrap(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId) {
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
	w.hostPlayerId = 0;
	w.flags        = 0;
	return w;
}

S_HeliGone ShotDownBy(uint8_t owner, uint16_t serial, uint8_t credit) {
	S_HeliGone g;
	InitHeader(g, 2000);
	g.ownerPlayerId       = owner;
	g.body                = HeliGoneBody{};
	g.body.serial         = serial;
	g.body.slot           = 0;
	g.body.reason         = HELI_GONE_SHOT_DOWN;
	g.body.creditPlayerId = credit;
	return g;
}

void TestTheHelicoptersRewardFollowsTheRule() {
	std::printf("\nthe $250 for somebody else's helicopter\n");
	g_heliPay = HeliPayRec{};
	WorldBridge b;
	b.heli.CreditHeliShootDown = &HeliCredit;
	b.heli.PayHeliShootDown    = &HeliPay;
	b.money                    = RecBridge();
	g_rec                      = Rec{};

	Client c;
	c.SetBridge(b);
	c.HandleMessage(Wrap(Welcome(1)));
	c.HandleMessage(Wrap(ShotDownBy(0, 7, 1)));
	Check(g_heliPay.credits == 1 && g_heliPay.paid == 0,
	      "money off: the crime and the statistics, and the $250 is nobody's");

	c.HandleMessage(Wrap(Pool(MONEY_RULE_OWN, false, 0, 0)));
	Check(c.MoneyForTest().Rule() == MONEY_RULE_OWN, "the client hands S_Money to the sync");
	c.HandleMessage(Wrap(ShotDownBy(0, 8, 1)));
	Check(g_heliPay.credits == 2 && g_heliPay.paid == 1, "own: the shooter is paid it");
	c.HandleMessage(Wrap(ShotDownBy(0, 9, 3)));
	Check(g_heliPay.paid == 1, "and nobody else's shoot-down pays us");
	S_HeliGone kept = ShotDownBy(0, 12, 1);
	kept.body.flags = HELI_GONE_OWNER_KEPT;
	c.HandleMessage(Wrap(kept));
	Check(g_heliPay.paid == 1 && g_heliPay.credits == 3 && g_heliPay.crimesOnly == 1,
	      "and when the owner's game kept the reward, the crime is all the shooter takes");

	c.ClearRosterForTest();
	Check(c.MoneyForTest().Rule() == MONEY_RULE_OFF && g_rec.rule == MONEY_RULE_OFF,
	      "a lost connection is back to off");
	c.HandleMessage(Wrap(Welcome(1)));
	c.HandleMessage(Wrap(ShotDownBy(0, 10, 1)));
	Check(g_heliPay.paid == 1, "and the next session pays nothing until it says otherwise");

	c.HandleMessage(Wrap(Pool(MONEY_RULE_SHARED, true, 5000, 0)));
	c.HandleMessage(Wrap(ShotDownBy(0, 11, 1)));
	Check(g_heliPay.paid == 2, "shared pays it too - into the wallet everybody has");
	c.HandleMessage(Wrap(AwardFor(1, 50, 0)));
	Check(g_rec.paid.size() == 1 && g_rec.paid[0] == 50,
	      "and the client hands S_MoneyAward to the sync");
}

// ---- the award function against the real exe -----------------------------------------

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

// Where an E8 rel32 at `site` lands.
uint32_t CallTarget(const std::vector<uint8_t> &img, uint32_t site) {
	return site + 5 + Dword(img, site + 1);
}

void TestTheAwardAgainstTheImage() {
	std::printf("\nthe award against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "award against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(img[AWARD_RETURN_FIRE_TIMER - 5 - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, AWARD_RETURN_FIRE_TIMER - 5) ==
	              CPlayerInfo__AwardMoneyForExplosion,
	      "the fire timer calls the award and returns to 0x005347A0");
	Check(img[AWARD_RETURN_BOMB_TIMER - 5 - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, AWARD_RETURN_BOMB_TIMER - 5) ==
	              CPlayerInfo__AwardMoneyForExplosion,
	      "the bomb timer calls it and returns to 0x00551D71");

	// Every E8 in the image that lands on it. Two, or the return-address test
	// in the detour is missing a caller.
	int callers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 &&
		    CallTarget(img, va) == CPlayerInfo__AwardMoneyForExplosion)
			++callers;
	Check(callers == 2, "and nothing else in the image calls it");

	// The fire timer blames +0x574, the bomb +0x218: mov eax,[ebp+574h] and
	// mov eax,[ebp+218h] right after each call.
	Check(img[0x005347A2 - IMAGE_BASE] == 0x8B && img[0x005347A3 - IMAGE_BASE] == 0x85 &&
	          Dword(img, 0x005347A4) == offs::AUTO_SET_ON_FIRE_ENTITY,
	      "the fire timer's culprit is m_pSetOnFireEntity");
	Check(img[0x00551D73 - IMAGE_BASE] == 0x8B && img[0x00551D74 - IMAGE_BASE] == 0x85 &&
	          Dword(img, 0x00551D75) == offs::VEH_BLOW_UP_ENTITY,
	      "the bomb's is m_pBlowUpEntity");

	// Inside the award: the chain, the handling field, the constant, the money.
	auto at = [&img](uint32_t va, std::initializer_list<uint8_t> bytes) {
		size_t i = 0;
		for (uint8_t b : bytes)
			if (img[va - IMAGE_BASE + i++] != b)
				return false;
		return true;
	};
	Check(at(0x004A15FC, {0x2B, 0x83}) && Dword(img, 0x004A15FE) ==
	                                          offs::PLAYERINFO_LAST_EXPLOSION_MS &&
	          at(0x004A1602, {0x3D}) && Dword(img, 0x004A1603) == EXPLOSION_CHAIN_MS,
	      "now - [this+104h] against 1770h, the six-second chain");
	Check(at(0x004A160D, {0xFF, 0x83}) &&
	          Dword(img, 0x004A160F) == offs::PLAYERINFO_EXPLOSION_CHAIN &&
	          at(0x004A1615, {0xC7, 0x83}) &&
	          Dword(img, 0x004A1617) == offs::PLAYERINFO_EXPLOSION_CHAIN &&
	          Dword(img, 0x004A161B) == 1,
	      "and [this+108h] one more, or back to one");
	Check(at(0x004A162A, {0x8B, 0x92}) && Dword(img, 0x004A162C) == offs::VEH_HANDLING &&
	          at(0x004A1630, {0x8B, 0x82}) &&
	          Dword(img, 0x004A1632) == offs::HANDLING_MONETARY_VALUE,
	      "one car's worth is pHandling->[+0D0h]");
	float factor = 0.0f;
	const uint32_t bits = Dword(img, 0x005F6AB4);
	std::memcpy(&factor, &bits, sizeof(factor));
	Check(at(0x004A1646, {0xD8, 0x0D}) && Dword(img, 0x004A1648) == 0x005F6AB4 &&
	          factor == EXPLOSION_REWARD_FACTOR,
	      "times the float at 005F6AB4h, which is 0.002f");
	Check(at(0x004A168A, {0x01, 0x2C, 0x85}) &&
	          Dword(img, 0x004A168D) == CWorld__Players + offs::PLAYERINFO_MONEY,
	      "into Players[PlayerInFocus].m_nMoney");

	// What the shared wallet writes over, and what the HUD reads instead.
	Check(at(0x00505E7B, {0x8B, 0x04, 0x95}) &&
	          Dword(img, 0x00505E7E) == CWorld__Players + offs::PLAYERINFO_VISIBLE_MONEY,
	      "the HUD prints m_nVisibleMoney");
	Check(at(0x0049FDC4, {0x8B, 0x8B}) &&
	          Dword(img, 0x0049FDC6) == offs::PLAYERINFO_VISIBLE_MONEY &&
	          at(0x0049FDCA, {0x8B, 0x83}) && Dword(img, 0x0049FDCC) == offs::PLAYERINFO_MONEY,
	      "and CPlayerInfo::Process walks it toward m_nMoney");

	// What a save stores of the cash (money.h, SaveWritesOwnMoney).
	Check(img[GenericSave_SavePlayerInfoCall - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, GenericSave_SavePlayerInfoCall) == CPlayerInfo__SavePlayerInfo &&
	          at(0x00590675, {0x81, 0xC1}) && Dword(img, 0x00590677) == CWorld__Players,
	      "GenericSave hands SavePlayerInfo &Players[PlayerInFocus]");
	int savers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && CallTarget(img, va) == CPlayerInfo__SavePlayerInfo)
			++savers;
	Check(savers == 1, "and nothing else calls it");
	Check(at(CPlayerInfo__SavePlayerInfo, {0x56, 0x57}) &&
	          at(0x004A0988, {0x81, 0xC1}) && Dword(img, 0x004A098A) == CWorld__Players &&
	          at(0x004A098E, {0x81, 0xC1}) && Dword(img, 0x004A0990) == offs::PLAYERINFO_MONEY &&
	          at(0x004A0994, {0x89, 0xCE, 0xA5}),
	      "which copies Players[PlayerInFocus].m_nMoney into the save");
	Check(at(0x004A0A24, {0x81, 0xC2}) && Dword(img, 0x004A0A26) == CWorld__Players &&
	          at(0x004A0A2A, {0x81, 0xC2}) &&
	          Dword(img, 0x004A0A2C) == offs::PLAYERINFO_VISIBLE_MONEY &&
	          at(0x004A0A30, {0x89, 0xD6, 0xA5}),
	      "and its m_nVisibleMoney");
	Check(img[0x00591F16 - IMAGE_BASE] == 0xE8 && CallTarget(img, 0x00591F16) == GenericSave,
	      "GenericSave is what the save screen calls");

	// The save's name (carlife.h, PickSaveName).
	Check(at(0x0058F8DA, {0xB9}) && Dword(img, 0x0058F8DB) == TheText &&
	          at(0x0058F8DF, {0x68}) && Dword(img, 0x0058F8E0) == CStats__LastMissionPassedName &&
	          img[0x0058F9AC - IMAGE_BASE] == 0xE8 && CallTarget(img, 0x0058F9AC) == CText__Get &&
	          at(0x0058F9B1, {0x85, 0xC0}) && at(0x0058F9B6, {0x74}),
	      "GenericSave names the save TheText.Get(LastMissionPassedName), unless that is null");
	Check(at(CText__Get, {0x8B, 0x44, 0x24, 0x04, 0x50, 0xE8}) &&
	          CallTarget(img, CText__Get + 5) == 0x0052BFB0 && at(CText__Get + 10, {0xC2, 0x04, 0x00}),
	      "CText::Get is the key array's search, __thiscall with one argument");
	Check(at(0x0052BFD0, {0x68}) && Dword(img, 0x0052BFD1) == 0x00600200 &&
	          std::memcmp(&img[0x00600200 - IMAGE_BASE], "%s missing", 11) == 0 &&
	          at(0x0052BFDE, {0x85, 0xF6, 0x75}) && at(0x0052C04C, {0xB8}) &&
	          Dword(img, 0x0052C04D) == CText__WideErrorString,
	      "which gives \"<key> missing\" in WideErrorString, never null, for a key it lacks");
	int keyWriters = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if ((img[va - IMAGE_BASE] == 0x68 || img[va - IMAGE_BASE] == 0xBE ||
		     img[va - IMAGE_BASE] == 0xBF) &&
		    Dword(img, va + 1) == CStats__LastMissionPassedName)
			++keyWriters;
	Check(at(0x00447FD0, {0x68}) && Dword(img, 0x00447FD1) == CStats__LastMissionPassedName &&
	          at(0x00447FCD, {0x6A, 0x08}) && at(0x00447FB7, {0xB9}) &&
	          Dword(img, 0x00447FB8) == TheText,
	      "REGISTER_MISSION_PASSED copies its eight bytes into the key");
	Check(keyWriters == 4 && at(0x004AB65F, {0xBE}) && at(0x004AB8DA, {0xBF}) &&
	          img[0x00590722 - IMAGE_BASE] == 0xE8 && CallTarget(img, 0x00590722) == 0x004AB3E0 &&
	          at(0x004AAF11, {0x0F, 0x7F, 0x80}) && Dword(img, 0x004AAF14) == CStats__LastMissionPassedName,
	      "and besides it only the stats' save (from GenericSave), load and Init touch it");

	// The fines a shared wallet pays, and the $250 a helicopter is worth.
	bool fines = at(0x004216CC, {0xFF, 0x24, 0x85}) &&
	             Dword(img, 0x004216CF) == BUSTED_FINE_TABLE;
	for (uint32_t level = 0; level < 7 && fines; ++level) {
		const uint32_t arm = Dword(img, BUSTED_FINE_TABLE + 4 * level);
		fines = img[arm - IMAGE_BASE] == 0xB8 && Dword(img, arm + 1) ==
		                                             uint32_t(BUSTED_FINES[level]);
	}
	Check(fines, "busted costs 100/100/200/400/600/900/1500 by wanted level");
	Check(at(0x004216FB, {0x80, 0xBB}) && Dword(img, 0x004216FD) == offs::PLAYERINFO_JAIL_FREE &&
	          at(0x004214D9, {0x80, 0xBB}) &&
	          Dword(img, 0x004214DB) == offs::PLAYERINFO_HOSPITAL_FREE &&
	          at(0x004214F6, {0x05}) && Dword(img, 0x004214F7) == uint32_t(-HOSPITAL_FEE),
	      "unless the jail or hospital flag is set, and wasted costs 1000");
	Check(at(0x0054A0DF, {0x81, 0x04, 0x8D}) &&
	          Dword(img, 0x0054A0E2) == CWorld__Players + offs::PLAYERINFO_MONEY &&
	          Dword(img, 0x0054A0E6) == uint32_t(HELI_REWARD_EACH.money),
	      "and UpdateHelis pays $250 into the same field");

	// The bomb timer's pay gate, the two calls game/money.cpp takes.
	Check(at(CVehicle__ProcessDelayedExplosion + 2, {0x89, 0xCD}),
	      "ProcessDelayedExplosion keeps the car in ebp");
	Check(at(0x00551D38, {0x66, 0x83, 0xBD}) && Dword(img, 0x00551D3B) == offs::VEH_BOMB_TIMER &&
	          at(0x00551D40, {0x75}),
	      "the gate is behind the fuse having run out");
	Check(img[BombTimer_FindPlayerVehicleCall - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, BombTimer_FindPlayerVehicleCall) == FindPlayerVehicle &&
	          at(0x00551D47, {0x39, 0xC5, 0x74, 0x26}),
	      "a car the bomber sits in pays nothing: FindPlayerVehicle() against ebp");
	Check(img[BombTimer_FindPlayerPedCall - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, BombTimer_FindPlayerPedCall) == FindPlayerPed &&
	          at(0x00551D50, {0x39, 0x85}) && Dword(img, 0x00551D52) == offs::VEH_BLOW_UP_ENTITY &&
	          at(0x00551D56, {0x75, 0x19}),
	      "and m_pBlowUpEntity has to be FindPlayerPed()");
	Check(img[BombTimer_AwardCall - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, BombTimer_AwardCall) == CPlayerInfo__AwardMoneyForExplosion &&
	          BombTimer_AwardCall + 5 == AWARD_RETURN_BOMB_TIMER && at(0x00551D5F, {0x55}),
	      "before the award for the car itself");
	Check(at(0x004A10D5, {0x80, 0xB9}) && Dword(img, 0x004A10D7) == offs::PED_IN_VEHICLE &&
	          at(0x004A10DE, {0x8B, 0x81}) && Dword(img, 0x004A10E0) == offs::PED_MY_VEHICLE,
	      "FindPlayerVehicle is the ped's bInVehicle and m_pMyVehicle, which the gate "
	      "reads off the bomber's");

	// And nothing but that award pays for what a bomb does: every instruction
	// that names m_nMoney by address.
	const uint32_t money   = CWorld__Players + offs::PLAYERINFO_MONEY;
	const uint32_t known[] = {
	    0x00422495, 0x00422A85, 0x00422A96, 0x00422C55, 0x00422E03, 0x00422E14, 0x004236DB,
	    0x00424140, 0x0042415E, 0x00426E98, 0x00426EE4, 0x00430EFA, 0x00430F20, 0x00431270,
	    0x004312B5, 0x0043132C, 0x0043DF35, 0x0043DF66, 0x0043DFA8, 0x00491446, 0x004A168A,
	    0x004A16B0, 0x004AB2B4, 0x004C021A, 0x004C041D, 0x004C042F, 0x004CED11, 0x004CED20,
	    0x004D67CD, 0x0052FDE7, 0x0054428F, 0x0054A0DF, 0x00551FAC, 0x0059641B, 0x005969B7};
	int named = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 4 < 0x005E4000; ++va)
		if (Dword(img, va) == money)
			++named;
	bool placed = true;
	for (uint32_t insn : known) {
		bool found = false;
		for (uint32_t off = 1; off <= 3 && !found; ++off)
			found = Dword(img, insn + off) == money;
		placed = placed && found;
	}
	Check(named == int(sizeof known / sizeof known[0]) && placed,
	      "35 instructions name it, and each is one of the known ones");
	bool noneInKills = true;
	for (uint32_t insn : known)
		if ((insn >= CPed__InflictDamage && insn < CPed__InflictDamage + 0x1000) ||
		    (insn >= CPed__SetDie && insn < CPed__SetDie + 0x400) ||
		    (insn >= CDarkel__RegisterKillByPlayer && insn < CDarkel__RegisterKillByPlayer + 0x110) ||
		    (insn >= CExplosion__AddExplosion && insn < CExplosion__AddExplosion + 0x800))
			noneInKills = false;
	Check(noneInKills,
	      "none of them in the damage, the death, the kill register or the explosion: a "
	      "bomb's kills pay nothing, as in single player");
}

// What the rampage reward watch, the kill statistics and the helicopter's
// take-back stand on, read back out of the exe.
void TestTheRewardSeamsAgainstTheImage() {
	std::printf("\nthe rampage reward, the kill statistics and the helicopter's $250, against "
	            "gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them\n");
		return;
	}
	auto at = [&img](uint32_t va, std::initializer_list<uint8_t> bytes) {
		size_t i = 0;
		for (uint8_t b : bytes)
			if (img[va - IMAGE_BASE + i++] != b)
				return false;
		return true;
	};

	// The reward watch's seam: one call, the one it redirects.
	int scriptCallers = 0;
	for (uint32_t va = IMAGE_BASE + 0x1000; va + 5 < IMAGE_BASE + IMAGE_SIZE; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && CallTarget(img, va) == CRunningScript__Process)
			++scriptCallers;
	Check(img[0x004393DF - IMAGE_BASE] == 0xE8 &&
	          CallTarget(img, 0x004393DF) == CRunningScript__Process && scriptCallers == 1,
	      "CTheScripts::Process's call at 0x004393DF is the only one to CRunningScript::Process");

	// The four statistics below the kill register's rampage branch.
	Check(at(0x00421013, {0xFF, 0x05}) && Dword(img, 0x00421015) == CStats__PeopleKilledByPlayer,
	      "RegisterKillByPlayer: PeopleKilledByPlayer");
	Check(at(0x00421021, {0x8A, 0x85}) && Dword(img, 0x00421023) == offs::PED_FLAGS_15B &&
	          at(0x00421027, {0xC0, 0xE8, 0x07}) &&
	          at(0x0042102E, {0xFF, 0x05}) &&
	          Dword(img, 0x00421030) ==
	              CStats__PedsKilledOfThisType + 4 * uint32_t(PEDTYPE_CRIMINAL),
	      "bChrisCriminal, the top bit of +15Bh, counts in the criminal row");
	Check(at(0x00421036, {0x8B, 0x85}) && Dword(img, 0x00421038) == offs::PED_TYPE &&
	          at(0x0042103C, {0xFF, 0x04, 0x85}) &&
	          Dword(img, 0x0042103F) == CStats__PedsKilledOfThisType,
	      "anybody else in his m_nPedType's row");
	Check(at(0x00421047, {0xFF, 0x05}) && Dword(img, 0x00421049) == CStats__HeadsPopped &&
	          at(0x0042104D, {0xFF, 0x05}) &&
	          Dword(img, 0x0042104F) == CStats__KillsSinceLastCheckpoint,
	      "then HeadsPopped for a headshot, and KillsSinceLastCheckpoint");
	Check(at(CDarkel__RegisterKillNotByPlayer, {0xFF, 0x05}) &&
	          Dword(img, CDarkel__RegisterKillNotByPlayer + 2) == CStats__PeopleKilledByOthers &&
	          at(CDarkel__RegisterKillNotByPlayer + 6, {0xC3}),
	      "and RegisterKillNotByPlayer is PeopleKilledByOthers and nothing else");

	// The helicopter: the slot goes before anything is paid.
	Check(at(0x0054A087, {0x8B, 0x85}) && at(0x0054A08D, {0xC7, 0x04, 0x85}) &&
	          Dword(img, 0x0054A090) == CHeli__pHelis && Dword(img, 0x0054A094) == 0,
	      "UpdateHelis nulls the exploding helicopter's slot at 0x0054A08D");
	bool noJumpPastPay = true;
	for (uint32_t va = 0x0054A098; va < 0x0054A0C1; ++va)
		if (img[va - IMAGE_BASE] == 0x74 || img[va - IMAGE_BASE] == 0x75)
			noJumpPastPay = noJumpPastPay && va + 2 + int8_t(img[va + 1 - IMAGE_BASE]) <= 0x0054A0C1;
	Check(noJumpPastPay && at(0x0054A0C1, {0xFF, 0x05}) &&
	          Dword(img, 0x0054A0C3) == CStats__HelisDestroyed,
	      "and nothing between there and the payment jumps past it");
}

} // namespace

int RunMoneyTests() {
	TestWhoDecidesAWreck();
	TestWhereAnAwardGoes();
	TestABombPaysItsBomber();
	TestTheEnginesArithmetic();
	TestThePoolArithmetic();
	TestOffIsNothing();
	TestOwnForwardsAwardsAndKeepsWallets();
	TestTheFirstPlayerSeedsTheSharedWallet();
	TestTheSharedWalletFollowsTheServer();
	TestANewPlayerIsNotAChange();
	TestASharedRampagePaysTheWalletOnce();
	TestARampageRewardInABusyFrame();
	TestAPoolThatEmptiesIsSeededAgain();
	TestOurOwnCashBesideTheWallet();
	TestAGroupPackagePaysTheWalletOnce();
	TestTheHelicoptersRewardFollowsTheRule();
	TestTheAwardAgainstTheImage();
	TestTheRewardSeamsAgainstTheImage();
	return g_moneyFailures;
}
