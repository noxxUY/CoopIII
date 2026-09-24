// What this machine knows about the vote before a rampage, and the line the
// help box shows for it. No engine here: Client keeps one of these, the game
// half (game/rampagevote.cpp) reads it once a frame, and tools/clienttest
// covers both without a game.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coopiii {

struct RampageVoteView {
	bool            seen = false;   // anything since the session started
	RampageVoteBody body{};
	uint32_t        atMs   = 0;     // our clock, when body arrived
	uint32_t        serial = 0;     // bumped on every packet

	// Whether we have pressed a key for this vote. A vote is final, the
	// server says so too; this only stops a second press being sent.
	uint8_t castFor  = 0;           // voteId, 0 for none
	bool    castYes  = false;

	void OnVote(const RampageVoteBody &b, uint32_t nowMs) {
		seen   = true;
		body   = b;
		atMs   = nowMs;
		++serial;
	}

	void Clear() { *this = RampageVoteView{}; }

	bool Open() const { return seen && body.state == RAMPAGE_VOTE_OPEN; }

	// Whole seconds left, rounded up, so the line says 1s for the last
	// second and never 0s while the vote is still open.
	uint32_t SecondsLeft(uint32_t nowMs) const {
		if (!Open())
			return 0;
		const uint32_t gone = nowMs - atMs;
		if (gone >= body.msLeft)
			return 0;
		return (body.msLeft - gone + 999u) / 1000u;
	}

	bool HaveCast() const { return Open() && castFor == body.voteId; }

	// May this machine press Y or N now? Not the toucher, whose yes is his
	// touch, and not twice.
	bool MayCast(uint8_t localId) const {
		return Open() && localId != body.starterId && !HaveCast();
	}

	void NoteCast(bool yes) {
		castFor = body.voteId;
		castYes = yes;
	}
};

// A key as the line should name it: the letter or digit itself, F1-F12, or
// "?" for anything else. Same set Config::ParseKey accepts.
inline void RampageKeyName(char *out, size_t cap, int vk) {
	if (cap == 0)
		return;
	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
		std::snprintf(out, cap, "%c", static_cast<char>(vk));
	else if (vk >= 0x70 && vk <= 0x7B)
		std::snprintf(out, cap, "F%d", vk - 0x70 + 1);
	else
		std::snprintf(out, cap, "?");
}

// A character the help box can draw and won't read as a token. CFont takes
// '~' as the start of a colour or key code, and it has no glyphs past ASCII.
inline wchar_t RampageGlyph(unsigned char c) {
	if (c == '~')
		return L'-';
	if (c >= 0x20 && c <= 0x7E)
		return static_cast<wchar_t>(c);
	return L'?';
}

// ASCII into the game's 16-bit text. Always terminated; returns the length.
inline size_t RampageWiden(wchar_t *out, size_t cap, const char *in) {
	if (cap == 0)
		return 0;
	size_t n = 0;
	for (; in && in[n] != '\0' && n + 1 < cap; ++n)
		out[n] = RampageGlyph(static_cast<unsigned char>(in[n]));
	out[n] = L'\0';
	return n;
}

// "<nick> wants to start a rampage. Press Y to vote yes or N to vote no.
// (1/2 yes, 12s)"
inline size_t FormatRampageVote(wchar_t *out, size_t cap, const char *nick, int yesKey,
                                int noKey, uint8_t yes, uint8_t voters,
                                uint32_t secondsLeft) {
	char ykey[8], nkey[8];
	RampageKeyName(ykey, sizeof ykey, yesKey);
	RampageKeyName(nkey, sizeof nkey, noKey);
	char raw[160];
	std::snprintf(raw, sizeof raw,
	              "%.*s wants to start a rampage. Press %s to vote yes or %s to vote no. "
	              "(%u/%u yes, %us)",
	              static_cast<int>(NICK_LEN - 1), nick && nick[0] ? nick : "Somebody", ykey,
	              nkey, static_cast<unsigned>(yes), static_cast<unsigned>(voters),
	              static_cast<unsigned>(secondsLeft));
	return RampageWiden(out, cap, raw);
}

inline const char *RampageVoteResultText(uint8_t state) {
	return state == RAMPAGE_VOTE_PASSED ? "Rampage vote passed" : "Rampage vote failed";
}

} // namespace coopiii
