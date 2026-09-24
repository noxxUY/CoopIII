// The vote before a rampage, the client's half: the help box's line, the view
// the game half reads, the ring round the toucher, who gets moved, and the
// skull's touch. game/rampagevote.cpp does the engine part and none of that
// runs here; what runs is every decision it makes.

#include "client.h"
#include "game/pickup.h"
#include "game/rampagevote.h"
#include "rampagevoteview.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_voteFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_voteFailures;
}

template <class T>
Message Wrap(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

std::string Narrow(const wchar_t *w) {
	std::string s;
	for (; *w; ++w)
		s.push_back(*w < 0x80 ? static_cast<char>(*w) : '#');
	return s;
}

RampageVoteBody Body(uint8_t id, uint8_t starter, uint8_t state, uint8_t yes, uint8_t voters,
                     uint16_t msLeft) {
	RampageVoteBody b{};
	b.voteId    = id;
	b.starterId = starter;
	b.state     = state;
	b.yes       = yes;
	b.voters    = voters;
	b.needed    = static_cast<uint8_t>((voters * 3 + 3) / 4);
	b.msLeft    = msLeft;
	return b;
}

void TestTheLine() {
	std::printf("\nthe help box line\n");
	wchar_t out[256];
	FormatRampageVote(out, 256, "alice", 'Y', 'N', 1, 2, 12);
	Check(Narrow(out) ==
	          "alice wants to start a rampage. Press Y to vote yes or N to vote no. (1/2 yes, 12s)",
	      "the line is word for word what the owner asked for");

	FormatRampageVote(out, 256, "bob", 'J', 0x71, 3, 4, 1);
	Check(Narrow(out) ==
	          "bob wants to start a rampage. Press J to vote yes or F2 to vote no. (3/4 yes, 1s)",
	      "and it names the keys the ini set, F-keys included");

	FormatRampageVote(out, 256, "a~r~b\xE9", 'Y', 'N', 1, 2, 5);
	Check(Narrow(out).compare(0, 5, "a-r-b") == 0 && Narrow(out)[5] == '?',
	      "a tilde can't start a CFont token and a byte past ASCII becomes '?'");

	FormatRampageVote(out, 256, nullptr, 'Y', 'N', 1, 2, 5);
	Check(Narrow(out).compare(0, 8, "Somebody") == 0, "no nick still says who");

	wchar_t tiny[8];
	const size_t n = FormatRampageVote(tiny, 8, "alice", 'Y', 'N', 1, 2, 5);
	Check(n == 7 && tiny[7] == L'\0', "a short buffer is cut and still terminated");

	char key[8];
	RampageKeyName(key, sizeof key, '7');
	Check(std::strcmp(key, "7") == 0, "a digit key is named as itself");
	RampageKeyName(key, sizeof key, 0x7B);
	Check(std::strcmp(key, "F12") == 0, "and F12 as F12");

	Check(std::strcmp(RampageVoteResultText(RAMPAGE_VOTE_PASSED), "Rampage vote passed") == 0 &&
	          std::strcmp(RampageVoteResultText(RAMPAGE_VOTE_FAILED), "Rampage vote failed") == 0 &&
	          std::strcmp(RampageVoteResultText(RAMPAGE_VOTE_CANCELLED), "Rampage vote failed") == 0,
	      "the two results, and a vote called off reads as failed");
}

void TestTheView() {
	std::printf("\nwhat the game half reads\n");
	RampageVoteView v;
	Check(!v.Open() && !v.MayCast(1), "nothing before any vote");

	v.OnVote(Body(3, 0, RAMPAGE_VOTE_OPEN, 1, 2, 15000), 1000);
	Check(v.Open() && v.SecondsLeft(1000) == 15, "open, 15 s at the start");
	Check(v.SecondsLeft(1001) == 15 && v.SecondsLeft(15000) == 1,
	      "rounded up, so the last second reads 1s and not 0s");
	Check(v.SecondsLeft(16000) == 0 && v.SecondsLeft(50000) == 0, "and 0 once it has run out");
	Check(!v.MayCast(0), "the toucher can't vote, his touch was his yes");
	Check(v.MayCast(1), "anybody else can");
	v.NoteCast(true);
	Check(!v.MayCast(1) && v.HaveCast(), "once");

	const uint32_t serial = v.serial;
	v.OnVote(Body(3, 0, RAMPAGE_VOTE_OPEN, 2, 2, 9000), 7000);
	Check(v.serial == serial + 1 && v.body.yes == 2 && v.SecondsLeft(7000) == 9,
	      "a count arriving restarts the countdown from what the server said");

	v.OnVote(Body(4, 1, RAMPAGE_VOTE_OPEN, 1, 3, 15000), 20000);
	Check(v.MayCast(2) && !v.HaveCast(), "a vote cast in the last one doesn't count in the next");

	v.OnVote(Body(4, 1, RAMPAGE_VOTE_PASSED, 3, 3, 0), 21000);
	Check(!v.Open() && !v.MayCast(2) && v.SecondsLeft(21000) == 0, "ended is not open");

	v.Clear();
	Check(!v.seen, "cleared");
}

void TestTheClientRoutesIt() {
	std::printf("\nthe vote through the client\n");
	Client c;

	S_Welcome w{};
	InitHeader(w, 1000);
	w.playerId   = 1;
	w.netId      = 101;
	w.maxPlayers = MAX_PLAYERS;
	w.snapshotHz = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	c.HandleMessage(Wrap(w));

	S_PlayerJoin j{};
	InitHeader(j, 1000);
	j.playerId = 0;
	j.netId    = 200;
	j.modelId  = 7;
	std::strncpy(j.nick, "alice", sizeof j.nick - 1);
	c.HandleMessage(Wrap(j));
	Check(c.NickFor(0) && std::strcmp(c.NickFor(0), "alice") == 0, "the toucher's name is known");
	Check(c.NickFor(5) == nullptr, "nobody's isn't");

	Check(!c.CastRampageVote(true), "no vote, nothing to send");

	S_RampageVote sv{};
	InitHeader(sv, 1000);
	sv.body = Body(9, 0, RAMPAGE_VOTE_OPEN, 1, 2, 15000);
	c.HandleMessage(Wrap(sv));
	Check(c.VoteView().Open() && c.VoteView().body.voteId == 9, "the vote reaches the view");
	Check(c.CastRampageVote(false), "a no goes out");
	Check(!c.CastRampageVote(true), "and a second press doesn't");

	RampageTeleportBody tb{};
	Check(!c.TakeRampageTeleport(tb), "no move until one is asked for");
	S_RampageTeleport st{};
	InitHeader(st, 1000);
	st.body.voteId    = 9;
	st.body.starterId = 0;
	st.body.slot      = 0;
	st.body.count     = 1;
	st.body.pos       = {10.0f, 20.0f, 30.0f};
	c.HandleMessage(Wrap(st));
	Check(c.TakeRampageTeleport(tb) && tb.pos.y == 20.0f && tb.starterId == 0,
	      "the move is handed over");
	Check(!c.TakeRampageTeleport(tb), "once");

	st.body.starterId = 1;
	c.HandleMessage(Wrap(st));
	Check(!c.TakeRampageTeleport(tb), "a move to ourselves is nothing");

	c.HandleMessage(Wrap(sv));
	c.HandleMessage(Wrap(w));
	Check(!c.VoteView().seen, "a new session forgets the old vote");
}

void TestTheRing() {
	std::printf("\nthe ring round the toucher\n");
	for (uint8_t count = 1; count <= MAX_PLAYERS - 1; ++count) {
		float closest = 1e9f;
		bool  onRing  = true;
		for (uint8_t a = 0; a < count; ++a) {
			const SpreadSpot p = SpreadCandidate(a, count, 0);
			onRing = onRing && std::fabs(std::hypot(p.dx, p.dy) - SPREAD_RADIUS_M) < 1e-4f;
			for (uint8_t b = a + 1; b < count; ++b) {
				const SpreadSpot q = SpreadCandidate(b, count, 0);
				closest = std::fmin(closest, std::hypot(p.dx - q.dx, p.dy - q.dy));
			}
		}
		char what[96];
		std::snprintf(what, sizeof what,
		              "%u around him: all 3 m out and at least 2.5 m apart", count);
		Check(onRing && (count == 1 || closest >= 2.5f), what);
	}

	bool inRange = true, allDifferent = true;
	for (int attempt = 0; attempt < SPREAD_ATTEMPTS; ++attempt) {
		const SpreadSpot s = SpreadCandidate(2, 5, attempt);
		const float      r = std::hypot(s.dx, s.dy);
		inRange = inRange && r >= 1.19f && r <= SPREAD_RADIUS_M * 1.5f + 1e-4f;
		for (int other = 0; other < attempt; ++other) {
			const SpreadSpot o = SpreadCandidate(2, 5, other);
			allDifferent = allDifferent && std::hypot(s.dx - o.dx, s.dy - o.dy) > 0.1f;
		}
	}
	Check(inRange, "every fallback stays between 1.2 m and 4.5 m from him");
	Check(allDifferent, "and every one is a different spot");
	const SpreadSpot z = SpreadCandidate(3, 0, 0);
	Check(std::isfinite(z.dx) && std::isfinite(z.dy), "a count of zero doesn't divide by it");

	Check(SpreadGroundOk(true, 10.0f, 11.5f) && !SpreadGroundOk(true, 10.0f, 12.5f),
	      "ground within 2 m of his is somewhere he can stand");
	Check(!SpreadGroundOk(false, 10.0f, 10.0f), "no ground is no ground");

	const float kPi = 3.14159265f;
	Check(std::fabs(HeadingToward(0, 0, 0, 5)) < 1e-5f, "facing north is heading 0");
	const float east = HeadingToward(0, 0, 5, 0);
	Check(std::fabs(east + kPi / 2) < 1e-5f && std::fabs(-std::sin(east) - 1.0f) < 1e-5f,
	      "facing east is -pi/2, and the engine's forward (-sin h, cos h) points at him");
}

void TestWhoMoves() {
	std::printf("\nwho is moved\n");
	TeleportFacts f;
	f.havePed = true;
	Check(DecideTeleport(f) == RAMPAGE_ARRIVED, "somebody walking about goes");

	TeleportFacts g = f;
	g.havePed = false;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_NO_PED, "no player, no move");
	g = f;
	g.health = 0.0f;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_DEAD, "dead stays");
	g = f;
	g.pedState = 48;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_DEAD, "dying stays");
	g = f;
	g.wbState = 1;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_DEAD, "wasted stays");
	g = f;
	g.wbState = 2;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_ARRESTED, "busted stays");
	g = f;
	g.pedState = 56;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_ARRESTED, "being cuffed stays");
	g = f;
	g.cutscene = true;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_CUTSCENE, "a cutscene stays");
	g = f;
	g.onMission = true;
	Check(DecideTeleport(g) == RAMPAGE_SKIPPED_MISSION, "a mission stays");
	g.frenzyOngoing = true;
	Check(DecideTeleport(g) == RAMPAGE_ARRIVED,
	      "but the flag rampage.sc sets for this very rampage is not a mission");

	Check(IslandOpen(1, false, false), "Portland is always open");
	Check(!IslandOpen(2, false, false) && IslandOpen(2, true, false),
	      "Staunton after portland_complete");
	Check(!IslandOpen(3, true, false) && IslandOpen(3, true, true),
	      "Shoreside after staunton_complete");
	Check(IslandOpen(0, false, false), "the water between is nobody's to lock");
}

void TestTheSkullsTouch() {
	std::printf("\nthe skull's touch\n");
	Check(SkullTouched(false, 0.5f, 0.5f, 1.9f), "on it, on foot");
	Check(!SkullTouched(true, 0.0f, 0.0f, 0.0f), "not from a car");
	Check(!SkullTouched(false, 0.0f, 0.0f, 2.0f) && !SkullTouched(false, 0.0f, 0.0f, -2.0f),
	      "two metres above or below is not a touch");
	Check(!SkullTouched(false, 1.0f, 0.9f, 0.0f), "1.81 flat is not a touch");
	Check(SkullTouched(false, 1.0f, 0.89f, 0.0f), "1.79 flat is");
	Check(SkullTouched(false, 0.0f, 0.0f, 0.0f) &&
	          !SkullTouched(false, kClaimRadius, 0.0f, 0.0f),
	      "the 4 m claim radius alone would have opened a vote from the pavement");
	Check(kSkullClaimTimeoutFrames > 15 * 60, "a skull claim outlives a vote");
	Check(kSkullEngineGraceFrames >= 6, "the engine gets its six-frame turn first");
	Check((PICKUP_F_RAMPAGE & PICKUP_F_BRIBE) == 0 && (PICKUP_F_VOTED & PICKUP_F_BRIBE) == 0 &&
	          (PICKUP_F_RAMPAGE & PICKUP_F_VOTED) == 0,
	      "three different bits");
}

} // namespace

int RunRampageVoteTests() {
	TestTheLine();
	TestTheView();
	TestTheClientRoutesIt();
	TestTheRing();
	TestWhoMoves();
	TestTheSkullsTouch();
	return g_voteFailures;
}
