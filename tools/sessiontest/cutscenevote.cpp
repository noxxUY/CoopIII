// Skipping a cutscene together, the server's half: server/core/cutscenevote.h.
// Who is in which scene, the count, the skip and who hears it, walked with no
// socket and no session.

#include "cutscenevote.h"

#include <cstdio>
#include <cstring>

using namespace coopiii;

namespace {

int g_skipFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_skipFailures;
}

CutsceneKey Key(uint8_t scope, const char *name) {
	CutsceneKey k{};
	k.scope = scope;
	std::strncpy(k.name, name, CUTSCENE_NAME_LEN);
	return k;
}

const CutsceneKey kIntro = Key(CUTSCENE_SCOPE_OWN, "bet");
const CutsceneKey kJoey  = Key(CUTSCENE_SCOPE_SHARED, "j1_lfl");

constexpr uint32_t Bit(uint8_t id) { return 1u << id; }

struct Sends {
	CutsceneVoteSend e[CutsceneVotes::MAX_SENDS];
	size_t           n = 0;

	void Take(CutsceneVotes &v, uint32_t now) { n = v.Evaluate(now, e, CutsceneVotes::MAX_SENDS); }

	const CutsceneVoteSend *Kind(CutsceneVoteSend::Kind k) const {
		for (size_t i = 0; i < n; ++i)
			if (e[i].kind == k)
				return &e[i];
		return nullptr;
	}
	size_t Count(CutsceneVoteSend::Kind k) const {
		size_t c = 0;
		for (size_t i = 0; i < n; ++i)
			c += e[i].kind == k;
		return c;
	}
};

void TestTheKey() {
	std::printf("\nwhich scene is which\n");
	Check(SameCutscene(Key(CUTSCENE_SCOPE_OWN, "BET"), kIntro), "the name without regard to case");
	Check(!SameCutscene(Key(CUTSCENE_SCOPE_SHARED, "bet"), kIntro),
	      "the same name in the session's mission is another scene");
	Check(!SameCutscene(Key(CUTSCENE_SCOPE_OWN, "bet2"), kIntro), "and so is another name");
	CutsceneKey full = Key(CUTSCENE_SCOPE_OWN, "abcdefgh");
	Check(SameCutscene(full, Key(CUTSCENE_SCOPE_OWN, "ABCDEFGH")), "eight letters, no terminator");
	Check(CutsceneVotesNeeded(2) == 2 && CutsceneVotesNeeded(3) == 3 && CutsceneVotesNeeded(4) == 3 &&
	          CutsceneVotesNeeded(8) == 6 && CutsceneVotesNeeded(1) == 1,
	      "75%, rounded up, as the rampage vote: 2 of 2, 3 of 3, 3 of 4, 6 of 8");
}

void TestTwoInTheIntro() {
	std::printf("\ntwo new games in the intro\n");
	CutsceneVotes v;
	Sends         s;
	v.Report(0, kIntro, 1000);
	s.Take(v, 1000);
	const CutsceneVoteSend *c = s.Kind(CutsceneVoteSend::COUNT);
	Check(s.n == 1 && c && c->to == Bit(0) && c->body.voters == 1,
	      "alone in it: he is told so, and skips on his own");

	v.Report(1, kIntro, 1100);
	s.Take(v, 1100);
	c = s.Kind(CutsceneVoteSend::COUNT);
	Check(c && c->to == (Bit(0) | Bit(1)) && c->body.voters == 2 && c->body.needed == 2 &&
	          c->body.yes == 0,
	      "the second one in: both hear 0 of 2, 2 needed");
	const uint8_t vote = c ? c->body.voteId : 0;
	Check(vote != 0 && v.VoteIdOf(0) == vote && v.VoteIdOf(1) == vote, "one count for both");

	s.Take(v, 1150);
	Check(s.n == 0, "nothing moved, nothing goes out");

	Check(!v.Cast(0, static_cast<uint8_t>(vote + 1)), "a press in another count is dropped");
	Check(v.Cast(0, vote), "A presses skip");
	Check(!v.Cast(0, vote), "and a second press is nothing");
	s.Take(v, 1200);
	c = s.Kind(CutsceneVoteSend::COUNT);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0 && c && c->body.yes == 1 && c->body.yesMask == Bit(0),
	      "1 of 2 is not enough: the count goes out, nobody skips");

	Check(v.Cast(1, vote), "B presses skip");
	s.Take(v, 1300);
	const CutsceneVoteSend *k = s.Kind(CutsceneVoteSend::SKIP);
	Check(k && k->to == (Bit(0) | Bit(1)) && k->body.yes == 2 && k->body.voters == 2 &&
	          SameCutscene(k->body.key, kIntro),
	      "2 of 2: both are told to skip, in the same breath");
	Check(v.Skipped(0) && v.Skipped(1), "and both are done with it");

	Check(!v.Cast(0, vote), "a press after the skip is nothing");
	s.Take(v, 1400);
	Check(s.n == 0, "still in the scene for a frame or two: nothing more goes out");

	v.Report(0, CutsceneKey{}, 1500);
	v.Report(1, CutsceneKey{}, 1500);
	s.Take(v, 1500);
	Check(s.n == 0, "out of it, nobody left to tell");

	v.Report(0, kIntro, 2000);
	v.Report(1, kIntro, 2000);
	s.Take(v, 2000);
	c = s.Kind(CutsceneVoteSend::COUNT);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0 && c && c->body.voters == 2 && c->body.yes == 0,
	      "the same scene again is a new count, not skipped for them");
}

void TestNobodyPresses() {
	std::printf("\nnobody presses\n");
	CutsceneVotes v;
	Sends         s;
	v.Report(0, kIntro, 0);
	v.Report(1, kIntro, 0);
	const uint8_t vote = v.VoteIdOf(0);
	v.Cast(0, vote);
	s.Take(v, 0);
	s.Take(v, 600000);
	Check(s.n == 0 && !v.Skipped(0) && !v.Skipped(1),
	      "no clock: ten minutes on, one yes of two still skips nothing");
	v.Report(1, CutsceneKey{}, 600001);
	s.Take(v, 600001);
	const CutsceneVoteSend *k = s.Kind(CutsceneVoteSend::SKIP);
	Check(k && k->to == Bit(0),
	      "B's scene ends on its own; A, alone with his yes, is skipped at once");
}

void TestFourPlayers() {
	std::printf("\nfour in the mission's scene\n");
	CutsceneVotes v;
	Sends         s;
	for (uint8_t id = 0; id < 4; ++id)
		v.Report(id, kJoey, 0);
	s.Take(v, 0);
	const CutsceneVoteSend *c = s.Kind(CutsceneVoteSend::COUNT);
	Check(c && c->body.voters == 4 && c->body.needed == 3 && c->to == 0xFu, "0 of 4, 3 needed");
	const uint8_t vote = v.VoteIdOf(0);
	v.Cast(0, vote);
	v.Cast(2, vote);
	s.Take(v, 10);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0, "two of four is not three");

	// D's replay goes (the scene failed to load there): three left, 2 of 3.
	v.Report(3, CutsceneKey{}, 20);
	s.Take(v, 20);
	c = s.Kind(CutsceneVoteSend::COUNT);
	Check(c && c->body.voters == 3 && c->body.needed == 3 && c->body.yes == 2 && c->to == 0x7u,
	      "one leaves: 2 of 3, 3 needed, and he hears nothing more");

	// A yes that leaves is not a yes any more.
	v.Leave(0);
	s.Take(v, 30);
	c = s.Kind(CutsceneVoteSend::COUNT);
	Check(c && c->body.voters == 2 && c->body.yes == 1 && c->body.yesMask == Bit(2),
	      "A leaves the session: his yes goes with him, 1 of 2");
	v.Cast(1, vote);
	s.Take(v, 40);
	const CutsceneVoteSend *k = s.Kind(CutsceneVoteSend::SKIP);
	Check(k && k->to == (Bit(1) | Bit(2)), "B says skip: both left skip");
}

void TestSeparateScenes() {
	std::printf("\nscenes that aren't the same one\n");
	CutsceneVotes v;
	Sends         s;
	v.Report(0, kIntro, 0);
	v.Report(1, Key(CUTSCENE_SCOPE_OWN, "hospital"), 0);
	v.Report(2, Key(CUTSCENE_SCOPE_SHARED, "bet"), 0);
	s.Take(v, 0);
	bool alone = s.n == 3;
	for (size_t i = 0; i < s.n; ++i)
		alone = alone && s.e[i].kind == CutsceneVoteSend::COUNT && s.e[i].body.voters == 1;
	Check(alone, "three players in three scenes: three counts of one, each skips alone");
	Check(v.VoteIdOf(0) != v.VoteIdOf(1) && v.VoteIdOf(0) != v.VoteIdOf(2), "three different counts");

	v.Report(1, kIntro, 10);
	s.Take(v, 10);
	const CutsceneVoteSend *c = s.Kind(CutsceneVoteSend::COUNT);
	Check(c && c->body.voters == 2 && c->to == (Bit(0) | Bit(1)),
	      "B moves into A's scene: the two of them are one count");
	Check(v.Cast(0, v.VoteIdOf(0)) && !v.Cast(2, v.VoteIdOf(0)),
	      "a press counts only in its own scene");

	Check(!v.Cast(5, 1) && !v.Cast(200, 1), "nobody in a scene can't vote");
	v.Report(200, kIntro, 20);
	Check(!v.In(200), "a slot past the table is ignored");
}

void TestLateIntoTheMissionsScene() {
	std::printf("\nlate into the mission's scene\n");
	CutsceneVotes v;
	Sends         s;
	v.Report(0, kJoey, 0);   // the owner
	v.Report(1, kJoey, 0);
	const uint8_t vote = v.VoteIdOf(0);
	v.Cast(0, vote);
	v.Cast(1, vote);
	s.Take(v, 100);
	Check(s.Kind(CutsceneVoteSend::SKIP) != nullptr, "owner and helper skip it");

	// C's replay only starts now, a second after the skip.
	v.Report(2, kJoey, 1100);
	s.Take(v, 1100);
	const CutsceneVoteSend *k = s.Kind(CutsceneVoteSend::SKIP);
	Check(k && k->to == Bit(2) && k->body.voteId == vote && k->body.voters == 0 &&
	          s.Count(CutsceneVoteSend::COUNT) == 0,
	      "C, a second late, is told to skip it too, alone, with no count");
	Check(v.Skipped(2), "and is done with it");

	// The helper's replay is cleared and loads the same scene again at once.
	v.Report(1, CutsceneKey{}, 1200);
	v.Report(1, kJoey, 1300);
	s.Take(v, 1300);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0, "somebody who was skipped once isn't skipped twice");

	// Past the grace, a newcomer is a new count.
	v.Report(3, kJoey, 1100 + CUTSCENE_SKIP_LATE_MS + 100);
	s.Take(v, 1100 + CUTSCENE_SKIP_LATE_MS + 100);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0 && !v.Skipped(3), "five seconds on, nobody is skipped");

	// A scene of one's own is never skipped for a latecomer.
	CutsceneVotes own;
	own.Report(0, kIntro, 0);
	own.Report(1, kIntro, 0);
	own.Cast(0, own.VoteIdOf(0));
	own.Cast(1, own.VoteIdOf(0));
	s.Take(own, 0);
	own.Report(2, kIntro, 500);
	s.Take(own, 500);
	Check(s.Count(CutsceneVoteSend::SKIP) == 0 && !own.Skipped(2),
	      "a third new game half a second into the intro plays it: nobody asked him");
}

} // namespace

int RunCutsceneVoteTests() {
	TestTheKey();
	TestTwoInTheIntro();
	TestNobodyPresses();
	TestFourPlayers();
	TestSeparateScenes();
	TestLateIntoTheMissionsScene();
	return g_skipFailures;
}
