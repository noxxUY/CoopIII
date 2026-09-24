// The vote before a rampage, the server's half: server/core/rampagevote.h.
// Every rule the owner set, walked with no socket and no session.

#include "rampagevote.h"

#include <cstdio>

using namespace coopiii;

namespace {

int g_voteFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_voteFailures;
}

using O = RampageVote::Outcome;

constexpr uint32_t Mask(uint8_t players) { return (1u << players) - 1u; }

PickupIdent Skull() {
	PickupIdent id{};
	id.pos        = {958.0f, -431.0f, 14.5f};
	id.modelIndex = 1361;
	id.type       = 3;
	id.flags      = PICKUP_F_RAMPAGE;
	return id;
}

void TestTheThreshold() {
	std::printf("\n75%%, rounded up\n");
	Check(RampageVotesNeeded(2) == 2, "two players: both");
	Check(RampageVotesNeeded(3) == 3, "three: all three");
	Check(RampageVotesNeeded(4) == 3, "four: three");
	Check(RampageVotesNeeded(5) == 4 && RampageVotesNeeded(6) == 5 &&
	          RampageVotesNeeded(7) == 6 && RampageVotesNeeded(8) == 6,
	      "five to eight: 4, 5, 6, 6");
	Check(RampageVotesNeeded(1) == 1, "one: himself");

	Check(!RampageNeedsVote(RAMPAGE_RULE_OFF, 4), "rampages off: no vote, everyone keeps their own");
	Check(!RampageNeedsVote(RAMPAGE_RULE_SHARED, 1), "alone: no vote");
	Check(RampageNeedsVote(RAMPAGE_RULE_SHARED, 2) && RampageNeedsVote(RAMPAGE_RULE_SCALED, 2),
	      "shared or scaled with two: a vote");
}

void TestTwoPlayers() {
	std::printf("\ntwo players\n");
	RampageVote v;
	Check(v.Evaluate(Mask(2), 0) == O::NONE, "nothing open, nothing to decide");
	Check(v.Start(0, Skull(), Mask(2), 1000), "A touches the skull");
	Check(v.IsOpen() && v.Yes() == 1 && v.Voters() == 2 && v.Needed() == 2,
	      "his touch is his yes: 1 of 2, 2 needed");
	Check(!v.Start(1, Skull(), Mask(2), 1100), "one vote at a time");
	Check(v.Evaluate(Mask(2), 1200) == O::OPEN, "still open");
	Check(v.Cast(1, v.Id(), true), "B says yes");
	Check(v.Evaluate(Mask(2), 1300) == O::PASSED && !v.IsOpen(), "and it passes");

	RampageVote w;
	w.Start(0, Skull(), Mask(2), 0);
	w.Cast(1, w.Id(), false);
	Check(w.Evaluate(Mask(2), 10) == O::FAILED_NO, "B says no: yes can't reach two, over at once");
}

void TestANoIsNotTheEnd() {
	std::printf("\na no ends it only when yes can't get there\n");
	RampageVote v;
	v.Start(0, Skull(), Mask(4), 0);
	v.Cast(1, v.Id(), false);
	Check(v.Evaluate(Mask(4), 10) == O::OPEN, "four players, one no: 3 can still be reached");
	v.Cast(2, v.Id(), true);
	Check(v.Evaluate(Mask(4), 20) == O::OPEN, "two yes, one no, one to go");
	v.Cast(3, v.Id(), true);
	Check(v.Evaluate(Mask(4), 30) == O::PASSED, "three of four: passed, with a no in it");

	RampageVote w;
	w.Start(0, Skull(), Mask(4), 0);
	w.Cast(1, w.Id(), false);
	w.Cast(2, w.Id(), false);
	Check(w.Evaluate(Mask(4), 10) == O::FAILED_NO, "two no out of four: 3 is out of reach");

	RampageVote x;
	x.Start(0, Skull(), Mask(3), 0);
	x.Cast(2, x.Id(), false);
	Check(x.Evaluate(Mask(3), 10) == O::FAILED_NO, "three players need all three: one no ends it");
}

void TestTheClock() {
	std::printf("\n15 seconds\n");
	RampageVote v;
	v.Start(0, Skull(), Mask(3), 5000);
	Check(v.MsLeft(5000) == 15000 && v.MsLeft(12000) == 8000, "counts down from 15 s");
	Check(v.Evaluate(Mask(3), 5000 + RAMPAGE_VOTE_MS - 1) == O::OPEN, "open to the last ms");
	Check(v.Evaluate(Mask(3), 5000 + RAMPAGE_VOTE_MS) == O::FAILED_TIME, "then out of time");
	Check(v.MsLeft(30000) == 0, "and nothing left");

	RampageVote w;
	w.Start(0, Skull(), Mask(2), 0xFFFFF000u);
	Check(w.Evaluate(Mask(2), 0xFFFFF000u + 1000u) == O::OPEN && w.MsLeft(0x00000100u) > 0,
	      "a server clock wrapping mid-vote reads as a short time, not a long one");
}

void TestCalledOff() {
	std::printf("\nthe toucher dies or leaves\n");
	RampageVote v;
	v.Start(1, Skull(), Mask(3), 0);
	v.CancelFor(2);
	Check(v.Evaluate(Mask(3), 10) == O::OPEN, "somebody else dying calls nothing off");
	v.CancelFor(1);
	Check(v.Evaluate(Mask(3), 20) == O::CANCELLED && !v.IsOpen(), "his own death does");

	RampageVote w;
	w.Start(1, Skull(), Mask(3), 0);
	w.Cast(0, w.Id(), true);
	Check(w.Evaluate(Mask(3) & ~(1u << 1), 10) == O::CANCELLED,
	      "him leaving calls it off, whatever the count");
}

void TestWhoCounts() {
	std::printf("\nwho is in the count\n");
	RampageVote v;
	v.Start(0, Skull(), Mask(3), 0);
	v.Cast(1, v.Id(), true);
	Check(v.Evaluate(Mask(3), 10) == O::OPEN, "2 of 3, three needed");
	Check(v.Evaluate(Mask(2), 20) == O::PASSED,
	      "C leaves: 2 of 2, and the vote is decided without him");

	RampageVote w;
	w.Start(0, Skull(), Mask(2), 0);
	Check(w.Evaluate(Mask(4), 10) == O::OPEN && w.Voters() == 2,
	      "two who join halfway aren't asked and don't count");
	Check(!w.Cast(3, w.Id(), true), "nor can they vote");
	Check(!w.Cast(0, w.Id(), true), "the toucher can't vote twice");
	Check(!w.Cast(1, w.Id() + 1, true), "a vote for another vote is dropped");
	Check(w.Cast(1, w.Id(), false) && !w.Cast(1, w.Id(), true), "a vote is final");

	RampageVote x;
	x.Start(0, Skull(), Mask(2), 0);
	Check(x.Evaluate(Mask(1), 10) == O::PASSED,
	      "everybody else gone: one of one, and he has said yes");
}

void TestWhatGoesOut() {
	std::printf("\nwhat the players are told\n");
	RampageVote v;
	v.Start(2, Skull(), Mask(4), 1000);
	Check(v.TakeDirty() && !v.TakeDirty(), "an opening is news once");
	RampageVoteBody b = v.Body(RAMPAGE_VOTE_OPEN, 4000);
	Check(b.voteId == v.Id() && b.starterId == 2 && b.yes == 1 && b.voters == 4 &&
	          b.needed == 3 && b.msLeft == 12000,
	      "open: who, 1 of 4, 3 needed, 12 s left");
	v.Cast(0, v.Id(), true);
	Check(v.TakeDirty(), "a vote is news");
	Check(v.Evaluate(Mask(4), 5000) == O::OPEN && !v.TakeDirty(), "a recount that changes nothing isn't");
	v.Cast(1, v.Id(), true);
	Check(v.Evaluate(Mask(4), 6000) == O::PASSED, "3 of 4");
	b = v.Body(RampageVoteStateOf(O::PASSED), 6000);
	Check(b.state == RAMPAGE_VOTE_PASSED && b.yes == 3 && b.voters == 4 && b.msLeft == 0,
	      "the ending carries the count it ended on");

	Check(RampageVoteStateOf(O::FAILED_NO) == RAMPAGE_VOTE_FAILED &&
	          RampageVoteStateOf(O::FAILED_TIME) == RAMPAGE_VOTE_FAILED &&
	          RampageVoteStateOf(O::CANCELLED) == RAMPAGE_VOTE_CANCELLED,
	      "both failures are one state on the wire, and a call-off is its own");

	const uint8_t first = v.Id();
	v.Start(0, Skull(), Mask(2), 7000);
	Check(v.Id() != first && v.Id() != 0, "every vote has its own id, and never 0");
	RampageVote w;
	for (int i = 0; i < 300; ++i) {
		w.Start(0, Skull(), Mask(2), 0);
		if (w.Id() == 0)
			break;
		w.CancelFor(0);
		w.Evaluate(Mask(2), 0);
	}
	Check(w.Id() != 0, "not even after it wraps");
	Check(w.Ident().modelIndex == 1361 && (w.Ident().flags & PICKUP_F_RAMPAGE) != 0,
	      "the skull it's about is kept, for the grant or the denial at the end");
}

} // namespace

int RunRampageVoteTests() {
	TestTheThreshold();
	TestTwoPlayers();
	TestANoIsNotTheEnd();
	TestTheClock();
	TestCalledOff();
	TestWhoCounts();
	TestWhatGoesOut();
	return g_voteFailures;
}
