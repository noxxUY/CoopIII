// The vote before a rampage (protocol.h, RampageVoteBody).
//
// All of it is arithmetic over who is in the session and who said what, with
// no socket and no session around it, so tools/sessiontest walks every rule
// here without a server. server.h does the sending.
//
// The rules:
//
//   - one vote at a time;
//   - it takes 75% of the players it was opened in front of, rounded up: two
//     players is both of them, three is three, four is three;
//   - the one who touched the skull has said yes by touching it;
//   - a no does not end it by itself, only once yes can't reach the mark any
//     more with everybody who hasn't voted yet;
//   - 15 seconds, then it has failed;
//   - it's called off if the toucher dies or leaves.
//
// Who can vote is fixed when it opens. Somebody who joins halfway is on a
// loading screen and has never seen the question, and counting him would
// hold up a vote nobody asked him. Somebody who leaves is taken out of the
// count, both ways: his vote goes and so does his place in the 75%.
#pragma once

#include "coopiii/protocol.h"

#include <cstdint>

namespace coopiii {

constexpr uint32_t RAMPAGE_VOTE_MS = 15000;

// Yes votes needed out of `voters`: 75%, rounded up.
constexpr uint8_t RampageVotesNeeded(uint8_t voters) {
	return static_cast<uint8_t>((static_cast<uint32_t>(voters) * 3u + 3u) / 4u);
}

// Whether a claim for a skull opens a vote at all. Not with rampages off,
// where every machine keeps its own frenzy, and not alone.
constexpr bool RampageNeedsVote(uint8_t rampageRule, uint8_t players) {
	return rampageRule != RAMPAGE_RULE_OFF && players >= 2;
}

constexpr uint8_t VoteBitCount(uint32_t mask) {
	uint8_t n = 0;
	for (; mask != 0; mask &= mask - 1)
		++n;
	return n;
}

class RampageVote {
public:
	enum class Outcome : uint8_t {
		NONE,        // no vote open
		OPEN,        // still going
		PASSED,
		FAILED_NO,   // yes can't get there any more
		FAILED_TIME,
		CANCELLED,   // the toucher died or left
	};

	bool               IsOpen() const { return m_open; }
	uint8_t            Id() const { return m_id; }
	uint8_t            Starter() const { return m_starter; }
	const PickupIdent &Ident() const { return m_ident; }

	// Opens a vote in front of everybody in `activeMask` (bit n is player id
	// n). False if one is already open.
	bool Start(uint8_t starter, const PickupIdent &ident, uint32_t activeMask,
	           uint32_t nowMs) {
		if (m_open || starter >= 32)
			return false;
		m_open    = true;
		m_starter = starter;
		m_ident   = ident;
		m_sinceMs = nowMs;
		m_voters  = activeMask | (1u << starter);
		m_yes     = 1u << starter;
		m_no      = 0;
		m_cancel  = false;
		m_dirty   = true;
		if (++m_id == 0)
			m_id = 1;
		return true;
	}

	// One player's say. False for no vote, the wrong vote, somebody who
	// isn't one of the voters, or somebody who has already voted - a vote is
	// final, the toucher's included.
	bool Cast(uint8_t player, uint8_t voteId, bool yes) {
		if (!m_open || voteId != m_id || player >= 32)
			return false;
		const uint32_t bit = 1u << player;
		if ((m_voters & bit) == 0 || ((m_yes | m_no) & bit) != 0)
			return false;
		(yes ? m_yes : m_no) |= bit;
		m_dirty = true;
		return true;
	}

	// The toucher died. Ends it at the next Evaluate.
	void CancelFor(uint8_t player) {
		if (m_open && player == m_starter)
			m_cancel = true;
	}

	// Recounts against who is still here and decides. Anything but OPEN or
	// NONE ends the vote, and is returned exactly once.
	Outcome Evaluate(uint32_t activeMask, uint32_t nowMs) {
		if (!m_open)
			return Outcome::NONE;

		const uint32_t before = m_voters;
		m_voters &= activeMask;
		if (m_voters != before)
			m_dirty = true;

		Outcome out = Outcome::OPEN;
		if (m_cancel || (activeMask & (1u << m_starter)) == 0)
			out = Outcome::CANCELLED;
		else if (Yes() >= Needed())
			out = Outcome::PASSED;
		else if (Yes() + Undecided() < Needed())
			out = Outcome::FAILED_NO;
		else if (nowMs - m_sinceMs >= RAMPAGE_VOTE_MS)
			out = Outcome::FAILED_TIME;

		if (out != Outcome::OPEN) {
			m_endedYes    = Yes();
			m_endedNo     = No();
			m_endedVoters = Voters();
			m_open        = false;
			m_dirty       = false;
		}
		return out;
	}

	// True once after anything changed that the players should see.
	bool TakeDirty() {
		const bool was = m_dirty;
		m_dirty        = false;
		return was;
	}

	uint8_t Voters() const { return VoteBitCount(m_voters); }
	uint8_t Needed() const { return RampageVotesNeeded(Voters()); }
	uint8_t Yes() const { return VoteBitCount(m_yes & m_voters); }
	uint8_t No() const { return VoteBitCount(m_no & m_voters); }
	uint8_t Undecided() const { return static_cast<uint8_t>(Voters() - Yes() - No()); }
	bool    IsVoter(uint8_t player) const { return player < 32 && (m_voters >> player & 1u); }

	uint16_t MsLeft(uint32_t nowMs) const {
		const uint32_t gone = nowMs - m_sinceMs;
		return gone >= RAMPAGE_VOTE_MS ? 0 : static_cast<uint16_t>(RAMPAGE_VOTE_MS - gone);
	}

	// What goes on the wire. While it's open, the live count; once it has
	// ended, the count it ended on.
	RampageVoteBody Body(uint8_t state, uint32_t nowMs) const {
		RampageVoteBody b{};
		b.voteId    = m_id;
		b.starterId = m_starter;
		b.state     = state;
		if (state == RAMPAGE_VOTE_OPEN) {
			b.yes    = Yes();
			b.voters = Voters();
			b.msLeft = MsLeft(nowMs);
		} else {
			b.yes    = m_endedYes;
			b.voters = m_endedVoters;
			b.msLeft = 0;
		}
		b.needed = RampageVotesNeeded(b.voters);
		return b;
	}

	uint8_t EndedNo() const { return m_endedNo; }

private:
	bool        m_open    = false;
	uint8_t     m_id      = 0;
	uint8_t     m_starter = INVALID_PLAYER;
	PickupIdent m_ident{};
	uint32_t    m_sinceMs = 0;
	uint32_t    m_voters  = 0;
	uint32_t    m_yes     = 0;
	uint32_t    m_no      = 0;
	bool        m_cancel  = false;
	bool        m_dirty   = false;
	uint8_t     m_endedYes    = 0;
	uint8_t     m_endedNo     = 0;
	uint8_t     m_endedVoters = 0;
};

inline uint8_t RampageVoteStateOf(RampageVote::Outcome outcome) {
	switch (outcome) {
	case RampageVote::Outcome::PASSED:    return RAMPAGE_VOTE_PASSED;
	case RampageVote::Outcome::CANCELLED: return RAMPAGE_VOTE_CANCELLED;
	case RampageVote::Outcome::FAILED_NO:
	case RampageVote::Outcome::FAILED_TIME: return RAMPAGE_VOTE_FAILED;
	default:                              return RAMPAGE_VOTE_OPEN;
	}
}

} // namespace coopiii
