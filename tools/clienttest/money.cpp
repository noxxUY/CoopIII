// Money in a session: who the engine half says an award belongs to, what
// MoneySync sends and writes for each rule, the $250 for a helicopter, and -
// when a copy of the retail exe is handed over - the award function and its
// two callers read back out of it.
//
// Nothing here burns a car. What runs is every decision either side of the
// engine: whether this machine may pay for a wreck at all, whom it pays, and
// what a shared wallet writes over the local cash when the total comes back.

#include "client.h"
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
}

} // namespace

int RunMoneyTests() {
	TestWhoDecidesAWreck();
	TestWhereAnAwardGoes();
	TestTheEnginesArithmetic();
	TestThePoolArithmetic();
	TestOffIsNothing();
	TestOwnForwardsAwardsAndKeepsWallets();
	TestTheFirstPlayerSeedsTheSharedWallet();
	TestTheSharedWalletFollowsTheServer();
	TestANewPlayerIsNotAChange();
	TestAPoolThatEmptiesIsSeededAgain();
	TestTheHelicoptersRewardFollowsTheRule();
	TestTheAwardAgainstTheImage();
	return g_moneyFailures;
}
