// What the server's own settings decide, walked with no socket: the vote
// rules for a cutscene and a rampage, the player limit, how long a session
// car nobody wants is kept, and the mission rules that go out in
// S_MissionState. servertest has who hears them.

#include "cutscenevote.h"
#include "rampagevote.h"
#include "session.h"

#include <cstdio>
#include <cstring>

using namespace coopiii;

namespace {

int g_settingFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_settingFailures;
}

constexpr uint32_t Mask(uint8_t players) { return (1u << players) - 1u; }

PickupIdent Skull() {
	PickupIdent id{};
	id.pos        = {958.0f, -431.0f, 14.5f};
	id.modelIndex = 1361;
	id.type       = 3;
	id.flags      = PICKUP_F_RAMPAGE;
	return id;
}

void TestTheVoteRules() {
	std::printf("\nhow many have to say yes, by rule\n");
	Check(VotesNeeded(VoteRule::Most, 4) == RampageVotesNeeded(4) &&
	          VotesNeeded(VoteRule::Most, 8) == 6,
	      "most is the 75% both votes always had");
	Check(VotesNeeded(VoteRule::Half, 2) == 1 && VotesNeeded(VoteRule::Half, 5) == 3 &&
	          VotesNeeded(VoteRule::Half, 8) == 4,
	      "half is half, rounded up: 1 of 2, 3 of 5, 4 of 8");
	Check(VotesNeeded(VoteRule::All, 3) == 3 && VotesNeeded(VoteRule::All, 8) == 8,
	      "all is every one of them");
	Check(VotesNeeded(VoteRule::Anyone, 8) == 1, "anyone is one");
	Check(VotesNeeded(VoteRule::Most, 1) == 1 && VotesNeeded(VoteRule::All, 1) == 1 &&
	          VotesNeeded(VoteRule::Half, 1) == 1,
	      "a player alone is always enough on his own");
	Check(VotesNeeded(VoteRule::Anyone, 0) == 0, "and nobody needs nobody");
}

void TestARampageVoteTakesTheRule() {
	std::printf("\na rampage vote under the host's rule\n");
	using O = RampageVote::Outcome;

	RampageVote anyone;
	anyone.SetRule(VoteRule::Anyone, 15000);
	anyone.Start(0, Skull(), Mask(4), 1000);
	Check(anyone.Needed() == 1 && anyone.Evaluate(Mask(4), 1001) == O::PASSED,
	      "anyone: the touch is enough, and it passes at once");

	RampageVote all;
	all.SetRule(VoteRule::All, 15000);
	all.Start(0, Skull(), Mask(3), 1000);
	all.Cast(1, all.Id(), true);
	Check(all.Needed() == 3 && all.Evaluate(Mask(3), 1100) == O::OPEN,
	      "all: two of three is not enough");
	all.Cast(2, all.Id(), false);
	Check(all.Evaluate(Mask(3), 1200) == O::FAILED_NO, "and one no ends it");

	RampageVote quick;
	quick.SetRule(VoteRule::Most, 5000);
	quick.Start(0, Skull(), Mask(2), 1000);
	Check(quick.Body(RAMPAGE_VOTE_OPEN, 1000).msLeft == 5000,
	      "the vote's time is the host's, and it goes out as msLeft");
	Check(quick.Evaluate(Mask(2), 5999) == O::OPEN && quick.IsOpen(),
	      "open to the last millisecond of it");
	Check(quick.Evaluate(Mask(2), 6000) == O::FAILED_TIME, "and then out of time");

	RampageVote clamp;
	clamp.SetRule(VoteRule::Most, 1000);
	Check(clamp.TimeMs() == RAMPAGE_VOTE_MS_MIN, "a time under 5 s is 5 s");
	clamp.SetRule(VoteRule::Most, 600000);
	Check(clamp.TimeMs() == RAMPAGE_VOTE_MS_MAX, "and one over a minute a minute, msLeft being 16 bits");

	RampageVote half;
	half.SetRule(VoteRule::Half, 15000);
	half.Start(0, Skull(), Mask(4), 1000);
	Check(half.Body(RAMPAGE_VOTE_OPEN, 1000).needed == 2, "what `needed` says is the rule's count");
}

void TestACutsceneSkipTakesTheRule() {
	std::printf("\na cutscene skip under the host's rule\n");
	CutsceneKey joey{};
	joey.scope = CUTSCENE_SCOPE_SHARED;
	std::strncpy(joey.name, "j1_lfl", CUTSCENE_NAME_LEN);

	CutsceneVotes v;
	v.SetRule(VoteRule::Anyone);
	for (uint8_t id = 0; id < 4; ++id)
		v.Report(id, joey, 1000);
	CutsceneVoteSend e[CutsceneVotes::MAX_SENDS];
	size_t           n = v.Evaluate(1000, e, CutsceneVotes::MAX_SENDS);
	Check(n >= 1 && e[0].kind == CutsceneVoteSend::COUNT && e[0].body.needed == 1,
	      "anyone: the count says one skip is enough");
	Check(v.Cast(2, v.VoteIdOf(2)), "one of the four presses skip");
	n = v.Evaluate(1100, e, CutsceneVotes::MAX_SENDS);
	bool skipped = false;
	for (size_t i = 0; i < n; ++i)
		skipped = skipped || (e[i].kind == CutsceneVoteSend::SKIP && e[i].to == Mask(4));
	Check(skipped, "and all four are told to skip");

	CutsceneVotes all;
	all.SetRule(VoteRule::All);
	all.Report(0, joey, 1000);
	all.Report(1, joey, 1000);
	all.Evaluate(1000, e, CutsceneVotes::MAX_SENDS);
	all.Cast(0, all.VoteIdOf(0));
	n = all.Evaluate(1100, e, CutsceneVotes::MAX_SENDS);
	bool early = false;
	for (size_t i = 0; i < n; ++i)
		early = early || e[i].kind == CutsceneVoteSend::SKIP;
	Check(!early && n >= 1 && e[0].body.needed == 2, "all: one of two is not a skip");
}

Player *Join(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

void TestThePlayerLimit() {
	std::printf("\nthe host's player limit\n");
	Session s;
	Check(s.PlayerLimit() == MAX_PLAYERS, "every slot by default");
	s.SetPlayerLimit(2);
	Check(Join(s, 1, "alice") && Join(s, 2, "bob"), "two come in");
	RejectReason reject = REJECT_NONE;
	Check(!s.AddPlayer(3, "carol", 7, PROTOCOL_VERSION, reject) && reject == REJECT_FULL,
	      "and a third is told it is full, with six slots empty");
	s.SetPlayerLimit(1);
	Check(s.Count() == 2, "lowering it under who is in turns nobody out");
	s.RemovePeer(2);
	Check(!Join(s, 3, "carol"), "and nobody new comes in until they are under it");
	s.SetPlayerLimit(0);
	Check(s.PlayerLimit() == 1, "a limit of nothing is one");
	s.SetPlayerLimit(40);
	Check(s.PlayerLimit() == MAX_PLAYERS, "and one past the slots is every slot");
	Check(Join(s, 3, "carol") != nullptr, "raised again, the next one comes in");
}

void TestTheMissionRulesGoOut() {
	std::printf("\nthe mission rules in S_MissionState\n");
	MissionSlot slot;
	S_MissionState st = slot.State();
	Check(st.checkpointWaitS == MISSION_CHECKPOINT_WAIT_MS / 1000 &&
	          st.catchUpM == MISSION_CATCH_UP_M_DEFAULT && st.behindM == MISSION_BEHIND_M_DEFAULT &&
	          st.behindS == MISSION_BEHIND_S_DEFAULT &&
	          (st.flags & MISSION_FLAG_TIMED_CHECKPOINTS) == 0,
	      "by default the numbers every client had built in");
	slot.SetCheckpointRules(20, true, 0, 300, 25);
	st = slot.State();
	Check(st.checkpointWaitS == 20 && st.catchUpM == 0 && st.behindM == 300 && st.behindS == 25 &&
	          (st.flags & MISSION_FLAG_TIMED_CHECKPOINTS) != 0 &&
	          (st.flags & MISSION_FLAG_FAIL_ON_DEATH) != 0,
	      "and the host's once set, next to the death rule");

	slot.Start(0, 21, 0x07);
	S_MissionWaiting w{};
	slot.TakeWaitingChange(&w);
	slot.Checkpoint(0, 0x02, {9.0f, 9.0f, 9.0f}, 50000);
	Check(slot.TakeWaitingChange(&w) && w.goesOnInS == 20,
	      "a checkpoint counts down the host's 20 s for everybody's screen");
	slot.Checkpoint(0, 0x06, {9.0f, 9.0f, 9.0f}, 50000 + 15000);
	Check(slot.TakeWaitingChange(&w) && w.missingMask == 0x06 && w.goesOnInS == 5,
	      "and it runs down from there, whoever else goes missing");

	Check(CheckpointsWait(21, false, 0, 60) && !CheckpointsWait(21, false, 0, 0),
	      "a story checkpoint waits, and no checkpoint waits with a wait of 0");
	Check(!CheckpointsWait(40, false, 0, 60) &&
	          CheckpointsWait(40, false, MISSION_FLAG_TIMED_CHECKPOINTS, 60),
	      "a race waits only when the host says races do");
	Check(!CheckpointsWait(21, true, 0, 60) &&
	          CheckpointsWait(21, true, MISSION_FLAG_TIMED_CHECKPOINTS, 60) &&
	          !CheckpointsWait(21, true, MISSION_FLAG_TIMED_CHECKPOINTS, 0),
	      "and so does one with a clock up, but still not with a wait of 0");
}

} // namespace

int RunSettingsTests() {
	TestTheVoteRules();
	TestARampageVoteTakesTheRule();
	TestACutsceneSkipTakesTheRule();
	TestThePlayerLimit();
	TestTheMissionRulesGoOut();
	return g_settingFailures;
}
