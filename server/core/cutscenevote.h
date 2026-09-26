// Skipping a cutscene together (protocol.h, CutsceneKey).
//
// All of it is arithmetic over who says they are in which cutscene and who
// has pressed skip, with no socket and no session around it, so
// tools/sessiontest walks every rule here without a server. server.h does
// the sending.
//
// The rules:
//
//   - the players in a cutscene are the ones whose game says it is in it,
//     scope and name alike, right now. A player who comes into it later is
//     counted from then on; one who leaves it, or the session, is taken out
//     of the count, his skip with him;
//   - it takes 75% of them, rounded up, the rampage vote's rule: two players
//     is both of them, four is three;
//   - pressing skip is a yes and stays one for as long as he is in it. There
//     is no no and no clock: a scene nobody skips ends on its own;
//   - once enough have said skip, everybody in it is told to skip at once;
//   - a player alone in a cutscene is told he is alone (one voter) and skips
//     on his own machine, as the game does;
//   - the session's mission's scene is one scene for its owner and every
//     participant: somebody whose replay of it starts within
//     CUTSCENE_SKIP_LATE_MS of its skip is told to skip it too. A scene of a
//     game's own is not, since nobody asked him.
#pragma once

#include "coopiii/protocol.h"
#include "rampagevote.h"

#include <cstdint>

namespace coopiii {

constexpr uint32_t CUTSCENE_SKIP_LATE_MS = 5000;

// 75% rounded up, the same count as the rampage vote.
constexpr uint8_t CutsceneVotesNeeded(uint8_t voters) { return RampageVotesNeeded(voters); }

// What the server has to send, worked out by CutsceneVotes::Evaluate.
struct CutsceneVoteSend {
	enum Kind : uint8_t { COUNT, SKIP };
	Kind             kind = COUNT;
	uint32_t         to   = 0;   // bit n: player n
	CutsceneVoteBody body{};     // for SKIP, voteId and key are what matter
};

class CutsceneVotes {
public:
	static constexpr size_t MAX_SENDS = MAX_PLAYERS * 2;

	// What `player`'s game is in now. CUTSCENE_SCOPE_NONE for nothing.
	void Report(uint8_t player, const CutsceneKey &key, uint32_t nowMs) {
		if (player >= MAX_PLAYERS)
			return;
		Seat &s = m_seat[player];
		if (key.scope == CUTSCENE_SCOPE_NONE) {
			if (s.in)
				Touch(s.key);
			s.in   = false;
			s.yes  = false;
			s.done = false;
			s.key  = CutsceneKey{};
			return;
		}
		if (s.in && SameCutscene(s.key, key))
			return;
		if (s.in)
			Touch(s.key);
		s.in   = true;
		s.yes  = false;
		s.done = false;
		s.key  = key;
		Group *g = FindOrMake(key);
		if (!g)
			return;
		g->dirty = true;
		// Late into the session's scene that was just skipped: skip it too.
		if (g->passed && key.scope == CUTSCENE_SCOPE_SHARED &&
		    nowMs - g->passedAtMs < CUTSCENE_SKIP_LATE_MS && s.gotPass != g->passSerial) {
			s.done    = true;
			s.gotPass = g->passSerial;
			m_late |= 1u << player;
		}
	}

	void Leave(uint8_t player) { Report(player, CutsceneKey{}, 0); }

	// A press of skip. False for somebody in no cutscene, the wrong vote, a
	// scene already skipped for him, or a second press.
	bool Cast(uint8_t player, uint8_t voteId) {
		if (player >= MAX_PLAYERS)
			return false;
		Seat &s = m_seat[player];
		if (!s.in || s.done || s.yes)
			return false;
		Group *g = Find(s.key);
		if (!g || g->voteId != voteId)
			return false;
		s.yes    = true;
		g->dirty = true;
		return true;
	}

	// Decides every cutscene whose count moved and says what to send. Anything
	// not sent now is not owed later: a count goes out whenever it changes.
	size_t Evaluate(uint32_t nowMs, CutsceneVoteSend *out, size_t max) {
		size_t n = 0;
		auto push = [&](const CutsceneVoteSend &e) {
			if (n < max)
				out[n++] = e;
		};
		for (Group &g : m_group) {
			if (!g.used)
				continue;

			// Whoever came in late to a skipped scene.
			uint32_t late = 0;
			for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
				if ((m_late >> id & 1u) && m_seat[id].in && SameCutscene(m_seat[id].key, g.key))
					late |= 1u << id;
			if (late) {
				m_late &= ~late;
				CutsceneVoteSend e;
				e.kind         = CutsceneVoteSend::SKIP;
				e.to           = late;
				e.body.voteId  = g.passedVoteId;
				e.body.key     = g.key;
				push(e);
			}

			const uint32_t members = Members(g);
			const uint32_t yesMask = YesOf(g);
			const uint8_t  voters  = VoteBitCount(members);
			const uint8_t  yes     = VoteBitCount(yesMask);
			if (members == 0) {
				if (!g.passed || nowMs - g.passedAtMs >= CUTSCENE_SKIP_LATE_MS)
					g = Group{};
				else
					g.dirty = false;
				continue;
			}

			if (yes > 0 && yes >= CutsceneVotesNeeded(voters)) {
				CutsceneVoteSend e;
				e.kind         = CutsceneVoteSend::SKIP;
				e.to           = members;
				e.body         = Body(g, members, yesMask);
				push(e);
				if (++m_passSerial == 0)
					m_passSerial = 1;
				for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
					if ((members >> id & 1u) == 0)
						continue;
					m_seat[id].done    = true;
					m_seat[id].yes     = false;
					m_seat[id].gotPass = m_passSerial;
				}
				g.passed       = true;
				g.passSerial   = m_passSerial;
				g.passedAtMs   = nowMs;
				g.passedVoteId = g.voteId;
				g.voteId       = NextId();
				g.dirty        = false;
				continue;
			}

			if (g.dirty) {
				CutsceneVoteSend e;
				e.kind = CutsceneVoteSend::COUNT;
				e.to   = members;
				e.body = Body(g, members, yesMask);
				push(e);
				g.dirty = false;
			}
		}
		return n;
	}

	// For the log and the tests.
	bool    In(uint8_t player) const { return player < MAX_PLAYERS && m_seat[player].in; }
	bool    Skipped(uint8_t player) const { return player < MAX_PLAYERS && m_seat[player].done; }
	uint8_t VoteIdOf(uint8_t player) const {
		if (player >= MAX_PLAYERS || !m_seat[player].in)
			return 0;
		const Group *g = Find(m_seat[player].key);
		return g ? g->voteId : 0;
	}
	uint8_t VotersWith(uint8_t player) const {
		if (player >= MAX_PLAYERS || !m_seat[player].in)
			return 0;
		const Group *g = Find(m_seat[player].key);
		return g ? VoteBitCount(Members(*g)) : 0;
	}
	const CutsceneKey &KeyOf(uint8_t player) const { return m_seat[player < MAX_PLAYERS ? player : 0].key; }

private:
	struct Seat {
		bool        in   = false;
		bool        yes  = false;
		bool        done = false;   // skipped for him; out of the count until he leaves it
		uint16_t    gotPass = 0;    // the last skip he was sent
		CutsceneKey key{};
	};
	struct Group {
		bool        used   = false;
		bool        dirty  = false;
		bool        passed = false;
		uint8_t     voteId = 0;
		uint8_t     passedVoteId = 0;
		uint16_t    passSerial   = 0;
		uint32_t    passedAtMs   = 0;
		CutsceneKey key{};
	};

	uint8_t NextId() {
		if (++m_nextId == 0)
			m_nextId = 1;
		return m_nextId;
	}

	Group *Find(const CutsceneKey &key) {
		for (Group &g : m_group)
			if (g.used && SameCutscene(g.key, key))
				return &g;
		return nullptr;
	}
	const Group *Find(const CutsceneKey &key) const {
		for (const Group &g : m_group)
			if (g.used && SameCutscene(g.key, key))
				return &g;
		return nullptr;
	}

	Group *FindOrMake(const CutsceneKey &key) {
		if (Group *g = Find(key))
			return g;
		for (Group &g : m_group) {
			if (g.used)
				continue;
			g        = Group{};
			g.used   = true;
			g.key    = key;
			g.voteId = NextId();
			return &g;
		}
		return nullptr;   // eight players can't fill eight groups and want a ninth
	}

	void Touch(const CutsceneKey &key) {
		if (Group *g = Find(key))
			g->dirty = true;
	}

	uint32_t Members(const Group &g) const {
		uint32_t mask = 0;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (m_seat[id].in && !m_seat[id].done && SameCutscene(m_seat[id].key, g.key))
				mask |= 1u << id;
		return mask;
	}
	uint32_t YesOf(const Group &g) const {
		uint32_t mask = 0;
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (m_seat[id].in && !m_seat[id].done && m_seat[id].yes &&
			    SameCutscene(m_seat[id].key, g.key))
				mask |= 1u << id;
		return mask;
	}

	static CutsceneVoteBody Body(const Group &g, uint32_t members, uint32_t yesMask) {
		CutsceneVoteBody b{};
		b.voteId  = g.voteId;
		b.voters  = VoteBitCount(members);
		b.yes     = VoteBitCount(yesMask);
		b.needed  = CutsceneVotesNeeded(b.voters);
		b.yesMask = static_cast<uint8_t>(yesMask & 0xFFu);
		b.key     = g.key;
		return b;
	}

	Seat     m_seat[MAX_PLAYERS];
	Group    m_group[MAX_PLAYERS];
	uint8_t  m_nextId     = 0;
	uint16_t m_passSerial = 0;
	uint32_t m_late       = 0;
};

} // namespace coopiii
