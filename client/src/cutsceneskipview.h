// What this machine knows about skipping the cutscene it is in, and what the
// skip input does about it. No engine here: Client keeps one of these, the
// game half (game/cutsceneskip.cpp) reads it once a frame and on every press,
// and tools/clienttest covers all of it without a game.
//
// server/core/cutscenevote.h has the count and protocol.h (CutsceneKey) the
// exchange.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coopiii {

struct CutsceneSkipView {
	// The cutscene we last told the server we are in; scope NONE for none.
	CutsceneKey reported{};

	// The last count for it. Kept only while it is about `reported`.
	bool             haveVote = false;
	CutsceneVoteBody vote{};
	uint8_t          castFor = 0;   // the voteId we said skip in, 0 for none

	// A skip the server ordered and the game half has not carried out yet.
	bool        skipWaiting = false;
	uint8_t     skipVoteId  = 0;
	CutsceneKey skipKey{};

	void Clear() { *this = CutsceneSkipView{}; }

	// A change of cutscene forgets the old count.
	void Report(const CutsceneKey &key) {
		if (!SameCutscene(key, reported)) {
			haveVote = false;
			castFor  = 0;
		}
		reported = key;
	}

	void OnVote(const CutsceneVoteBody &b) {
		if (reported.scope == CUTSCENE_SCOPE_NONE || !SameCutscene(b.key, reported))
			return;   // about a scene we have left already
		haveVote = true;
		vote     = b;
	}

	void OnSkip(uint8_t voteId, const CutsceneKey &key) {
		skipWaiting = true;
		skipVoteId  = voteId;
		skipKey     = key;
	}

	bool In() const { return reported.scope != CUTSCENE_SCOPE_NONE; }

	// Somebody else is in our cutscene: the skip input is a vote.
	bool Crowded() const { return In() && haveVote && vote.voters >= 2; }

	bool HaveCast() const { return haveVote && castFor != 0 && castFor == vote.voteId; }

	void NoteCast() {
		if (haveVote)
			castFor = vote.voteId;
	}
};

// ---- what the skip input does ------------------------------------------------

enum class SkipPress : uint8_t {
	Local,     // skip it here, as the game does
	Vote,      // say skip, and wait for the others
	Nothing,   // said already, or not ours to skip
};

// `follower`: a participant replaying the session's mission's scene, whose
// skip only ever came from its owner. Everybody else alone in a cutscene
// skips it on their own.
inline SkipPress DecideSkipPress(bool connected, uint8_t scope, bool follower, bool crowded,
                                 bool haveCast) {
	if (!connected || scope == CUTSCENE_SCOPE_NONE)
		return SkipPress::Local;
	if (crowded)
		return haveCast ? SkipPress::Nothing : SkipPress::Vote;
	if (scope == CUTSCENE_SCOPE_SHARED && follower)
		return SkipPress::Nothing;
	return SkipPress::Local;
}

// Whose cutscene this game is in: the session's mission's, on its owner or on
// a participant replaying it, unless the game is busy with a mission of its
// own; otherwise its own.
inline uint8_t CutsceneScopeFor(bool sessionMissionRunning, bool busy, bool owner,
                                bool participant) {
	if (sessionMissionRunning && !busy && (owner || participant))
		return CUTSCENE_SCOPE_SHARED;
	return CUTSCENE_SCOPE_OWN;
}

// ms_cutsceneName as the wire has it: eight bytes, lower case, zeros after
// the end.
inline CutsceneKey MakeCutsceneKey(uint8_t scope, const char *name) {
	CutsceneKey k{};
	k.scope = scope;
	for (size_t i = 0; i < CUTSCENE_NAME_LEN && name && name[i] != '\0'; ++i) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
		k.name[i] = c;
	}
	return k;
}

// The engine's own "not the credits" test, faststricmp(name, "end").
inline bool IsCreditsCutscene(const char *name) {
	const char *end = "end";
	for (size_t i = 0;; ++i) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
		if (c != end[i])
			return false;
		if (c == '\0')
			return true;
	}
}

// The name for the log: printable, terminated.
inline void CutsceneKeyName(const CutsceneKey &k, char (&out)[CUTSCENE_NAME_LEN + 1]) {
	size_t n = 0;
	for (; n < CUTSCENE_NAME_LEN && k.name[n] != '\0'; ++n)
		out[n] = k.name[n] >= ' ' && k.name[n] <= '~' ? k.name[n] : '?';
	out[n] = '\0';
}

// "Skip 1/2 - press Enter" until we have said skip, "Skip 1/2" after. Enter
// is one of the five inputs CCutsceneMgr::Update takes, and the one every
// keyboard has in the same place.
inline size_t FormatSkipCounter(char *out, size_t cap, uint8_t yes, uint8_t voters, bool haveCast) {
	if (cap == 0)
		return 0;
	const int n = std::snprintf(out, cap, haveCast ? "Skip %u/%u" : "Skip %u/%u - press Enter",
	                            static_cast<unsigned>(yes), static_cast<unsigned>(voters));
	if (n < 0) {
		out[0] = '\0';
		return 0;
	}
	return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

} // namespace coopiii
