// The chat and the session's notices, the way the HUD shows them: what was
// said, who came and went, the line being typed, and the version mark in the
// corner. No engine in here, so tools/clienttest walks all of it;
// game/chat.cpp draws it and reads the keys.
#pragma once

#include <coopiii/protocol.h>
#include <coopiii/version.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coopiii {

// A line stays up this long after it arrives and spends the last part of it
// fading. While the player is typing every line held is shown in full.
constexpr uint32_t FEED_SHOW_MS = 10000;
constexpr uint32_t FEED_FADE_MS = 1000;
constexpr size_t   FEED_LINES   = 10;

// A message longer than this goes on as many lines as it takes, broken at a
// space where there is one. Short enough that a line of wide letters still
// fits FEED_WIDTH_OF_WIDTH on a 4:3 screen; anything that somehow does not is
// cut at the edge when it is drawn.
constexpr size_t FEED_WRAP = 56;
constexpr size_t FEED_TEXT = FEED_WRAP + 1;

// Room for a whole message before it is broken up: the longest name, its
// colon and space, and the longest text the packet carries.
constexpr size_t FEED_MESSAGE = NICK_LEN + CHAT_LEN + 4;

// What goes before a line that carries on the one above it.
constexpr const char *FEED_CONTINUED = "  ";

enum class FeedKind : uint8_t { Chat, Notice };

struct FeedLine {
	FeedKind kind     = FeedKind::Notice;
	uint32_t atMs     = 0;
	// Whose line, for the colour of the name at the start of it, and how many
	// characters that name takes (colon included). INVALID_PLAYER and 0 for a
	// notice or the rest of a long message.
	uint8_t  playerId = INVALID_PLAYER;
	uint8_t  nickLen  = 0;
	char     text[FEED_TEXT] = {};
};

// One character, made safe for CFont. The rules are nametag.h's TagGlyph's
// without the uppercasing, for the same reasons: CFont indexes its width table
// with `c - ' '`, '~' opens a formatting token that eats the rest of the line,
// and a string whose first character is '*' is not drawn at all.
inline char FeedGlyph(unsigned char c, bool first) {
	if (c == '~')
		return '-';
	if (first && c == '*')
		return '+';
	if (c < ' ' || c > '~')
		return '?';
	return static_cast<char>(c);
}

inline void FeedCopy(char *out, size_t cap, const char *in) {
	if (cap == 0)
		return;
	size_t n = 0;
	for (; in && in[n] != '\0' && n + 1 < cap; ++n)
		out[n] = FeedGlyph(static_cast<unsigned char>(in[n]), n == 0);
	out[n] = '\0';
}

// A typed character as something CFont can draw and the wire carries: itself
// for plain ASCII, the letter without its accent for the Latin ones a
// Spanish, French, German or Portuguese keyboard types, the nearest ASCII for
// the typographic quotes and dashes a paste brings in, and 0 for anything else.
// '~' comes out as '-' here already, so the line being typed is never a
// formatting token either.
inline char ChatGlyph(uint32_t c) {
	if (c == '~')
		return '-';
	if (c >= 0x20 && c <= 0x7E)
		return static_cast<char>(c);
	if (c == 0xA0)
		return ' ';
	if (c == 0xA1)
		return '!';
	if (c == 0xBF)
		return '?';
	if (c == 0xAB || c == 0xBB || c == 0x201C || c == 0x201D || c == 0x201E)
		return '"';
	if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0xB4)
		return '\'';
	if (c == 0x2013 || c == 0x2014)
		return '-';
	if (c == 0x2026)
		return '.';
	if (c >= 0xC0 && c <= 0xFF) {
		// Latin-1's letters, upper then lower, a row of the base letter for
		// each. 0xD7 and 0xF7 are the multiplication and division signs.
		static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
		                              "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
		return kLatin1[c - 0xC0];
	}
	return 0;
}

// "nick: text". The nick is cut at NICK_LEN and the text at what the packet
// carries; `nickLen` is how much of the result is the name and its colon.
inline void FormatChatLine(char *out, size_t cap, const char *nick, const char *text,
                           size_t *nickLen = nullptr) {
	char raw[FEED_MESSAGE];
	const int n = std::snprintf(raw, sizeof raw, "%.*s:", static_cast<int>(NICK_LEN - 1),
	                            nick && nick[0] ? nick : "?");
	std::snprintf(raw + n, sizeof raw - static_cast<size_t>(n), " %.*s",
	              static_cast<int>(CHAT_LEN - 1), text ? text : "");
	FeedCopy(out, cap, raw);
	if (nickLen)
		*nickLen = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

inline void FormatJoined(char *out, size_t cap, const char *nick) {
	char raw[NICK_LEN + 16];
	std::snprintf(raw, sizeof raw, "%.*s joined", static_cast<int>(NICK_LEN - 1),
	              nick ? nick : "?");
	FeedCopy(out, cap, raw);
}

inline void FormatLeft(char *out, size_t cap, const char *nick, uint8_t reason) {
	const char *how = reason == LEAVE_TIMEOUT ? "lost connection"
	                  : reason == LEAVE_KICKED ? "was kicked"
	                                           : "left";
	char raw[NICK_LEN + 24];
	std::snprintf(raw, sizeof raw, "%.*s %s", static_cast<int>(NICK_LEN - 1),
	              nick ? nick : "?", how);
	FeedCopy(out, cap, raw);
}

// Where the next line of a long message ends: at FEED_WRAP characters, pulled
// back to the last space if there is one in the back two thirds of the line,
// so a word is only split when it is longer than that.
inline size_t FeedBreak(const char *text, size_t len, size_t room) {
	if (len <= room)
		return len;
	for (size_t i = room; i > room / 3; --i)
		if (text[i] == ' ')
			return i;
	return room;
}

// The last FEED_LINES lines, oldest first.
class ChatFeed {
public:
	// A message, on as many lines as it takes. The first carries the name;
	// the rest are indented under it.
	void Push(FeedKind kind, const char *text, uint32_t nowMs,
	          uint8_t playerId = INVALID_PLAYER, size_t nickLen = 0) {
		if (!text)
			return;
		size_t len = std::strlen(text);
		size_t at  = 0;
		bool   first = true;
		while (first || at < len) {
			const size_t lead = first ? 0 : std::strlen(FEED_CONTINUED);
			const size_t take = FeedBreak(text + at, len - at, FEED_WRAP - lead);

			FeedLine &line = Next();
			line.kind     = kind;
			line.atMs     = nowMs;
			line.playerId = first ? playerId : INVALID_PLAYER;
			line.nickLen  = first && nickLen <= take ? static_cast<uint8_t>(nickLen) : 0;

			char raw[FEED_TEXT];
			std::memcpy(raw, FEED_CONTINUED, lead);
			std::memcpy(raw + lead, text + at, take);
			raw[lead + take] = '\0';
			FeedCopy(line.text, sizeof line.text, raw);

			at += take;
			while (at < len && text[at] == ' ')
				++at;   // the space a line was broken at starts nothing
			first = false;
		}
	}

	void Clear() {
		m_head  = 0;
		m_count = 0;
	}

	size_t Count() const { return m_count; }
	const FeedLine &Line(size_t i) const { return m_lines[(m_head + i) % FEED_LINES]; }

private:
	FeedLine &Next() {
		if (m_count == FEED_LINES) {
			m_head = (m_head + 1) % FEED_LINES;
			--m_count;
		}
		FeedLine &line = m_lines[(m_head + m_count) % FEED_LINES];
		line = FeedLine{};
		++m_count;
		return line;
	}

	FeedLine m_lines[FEED_LINES];
	size_t   m_head  = 0;
	size_t   m_count = 0;
};

// How opaque a line of this age is. Unsigned, so a WallClock wrap reads as a
// line that is still new.
inline uint8_t FeedAlpha(uint32_t ageMs, bool typing) {
	if (typing)
		return 255;
	if (ageMs >= FEED_SHOW_MS)
		return 0;
	const uint32_t left = FEED_SHOW_MS - ageMs;
	if (left >= FEED_FADE_MS)
		return 255;
	return static_cast<uint8_t>(left * 255u / FEED_FADE_MS);
}

// Lines this player sent, newest last, for the arrow keys.
constexpr size_t CHAT_HISTORY = 10;

// The line being typed, with a caret the arrow keys move and the lines already
// sent a key away. Never more than the packet carries, and never a character
// ChatGlyph turns down.
class ChatLine {
public:
	bool Open() const { return m_open; }

	void Begin() {
		m_open    = true;
		m_len     = 0;
		m_caret   = 0;
		m_buf[0]  = '\0';
		m_recall  = m_historyCount;
		m_draft[0] = '\0';
	}

	void Cancel() {
		m_open   = false;
		m_len    = 0;
		m_caret  = 0;
		m_buf[0] = '\0';
	}

	// False when it was not taken: not typing, nothing ChatGlyph draws, or full.
	bool Type(uint32_t ch) {
		const char c = ChatGlyph(ch);
		if (!m_open || c == 0 || m_len + 1 >= CHAT_LEN)
			return false;
		std::memmove(m_buf + m_caret + 1, m_buf + m_caret, m_len - m_caret + 1);
		m_buf[m_caret++] = c;
		++m_len;
		return true;
	}

	// A paste: every character Type takes, line breaks and tabs as spaces,
	// until the line is full. How many went in.
	template <class Char>
	size_t Paste(const Char *text) {
		size_t n = 0;
		for (; m_open && text && *text; ++text) {
			uint32_t c = static_cast<uint32_t>(*text);
			if (sizeof(Char) == 1)
				c &= 0xFF;
			if (c == '\r')
				continue;
			if (c == '\n' || c == '\t')
				c = ' ';
			if (m_len + 1 >= CHAT_LEN)
				break;
			if (Type(c))
				++n;
		}
		return n;
	}

	void Backspace() {
		if (!m_open || m_caret == 0)
			return;
		--m_caret;
		EraseAtCaret();
	}

	void Delete() {
		if (m_open && m_caret < m_len)
			EraseAtCaret();
	}

	void Clear() {
		if (!m_open)
			return;
		m_len    = 0;
		m_caret  = 0;
		m_buf[0] = '\0';
	}

	void Left()  { if (m_caret > 0) --m_caret; }
	void Right() { if (m_caret < m_len) ++m_caret; }
	void Home()  { m_caret = 0; }
	void End()   { m_caret = m_len; }

	// Up and Down, the way a shell has them: back through what was sent, and
	// forward again to what was being typed before the first Up.
	void Older() {
		if (!m_open || m_recall == 0)
			return;
		if (m_recall == m_historyCount)
			std::memcpy(m_draft, m_buf, m_len + 1);
		--m_recall;
		Load(m_history[m_recall]);
	}

	void Newer() {
		if (!m_open || m_recall >= m_historyCount)
			return;
		++m_recall;
		Load(m_recall == m_historyCount ? m_draft : m_history[m_recall]);
	}

	// Closes the line and hands over what was typed, remembering it for Up.
	// False, with nothing handed over, for a line of nothing but spaces.
	bool Submit(char (&out)[CHAT_LEN]) {
		bool any = false;
		for (size_t i = 0; i < m_len; ++i)
			if (m_buf[i] != ' ')
				any = true;
		if (any) {
			// All of it: the whole buffer goes on the wire.
			std::memset(out, 0, CHAT_LEN);
			std::memcpy(out, m_buf, m_len + 1);
			Remember(m_buf);
		}
		Cancel();
		return any;
	}

	const char *Text() const { return m_buf; }
	size_t Length() const { return m_len; }
	size_t Caret() const { return m_caret; }
	size_t HistoryCount() const { return m_historyCount; }

private:
	void EraseAtCaret() {
		std::memmove(m_buf + m_caret, m_buf + m_caret + 1, m_len - m_caret);
		--m_len;
	}

	void Load(const char *text) {
		size_t n = std::strlen(text);
		if (n >= CHAT_LEN)
			n = CHAT_LEN - 1;
		std::memcpy(m_buf, text, n);
		m_buf[n] = '\0';
		m_len    = n;
		m_caret  = n;
	}

	// The same line sent twice in a row is one entry; the oldest goes when
	// the list is full.
	void Remember(const char *text) {
		if (m_historyCount > 0 && std::strcmp(m_history[m_historyCount - 1], text) == 0)
			return;
		if (m_historyCount == CHAT_HISTORY) {
			std::memmove(m_history[0], m_history[1], sizeof m_history[0] * (CHAT_HISTORY - 1));
			--m_historyCount;
		}
		std::memcpy(m_history[m_historyCount++], text, std::strlen(text) + 1);
	}

	bool   m_open  = false;
	size_t m_len   = 0;
	size_t m_caret = 0;
	char   m_buf[CHAT_LEN] = {};

	char   m_history[CHAT_HISTORY][CHAT_LEN] = {};
	size_t m_historyCount = 0;
	size_t m_recall       = 0;   // m_historyCount means the draft
	char   m_draft[CHAT_LEN] = {};
};

// Each player's name in the chat, by slot, so a conversation reads at a glance.
// Picked to stand apart from each other, from the white of the text and from
// the orange of the notices.
struct NickColour {
	uint8_t r, g, b;
};

inline NickColour ChatNickColour(uint8_t playerId) {
	static const NickColour kPalette[MAX_PLAYERS] = {
	    {232, 93, 69},   {92, 170, 235}, {120, 200, 95}, {235, 200, 70},
	    {190, 120, 225}, {80, 210, 200}, {240, 140, 180}, {215, 160, 105},
	};
	return playerId < MAX_PLAYERS ? kPalette[playerId] : NickColour{200, 200, 200};
}

// Where the feed goes, from the screen size alone: the left edge, from a
// little above the radar upwards, in HUD units of screen height so it is the
// same fraction of the screen at any resolution (game/nametag.h, MeasureTag,
// has why height and not width).
struct FeedLayout {
	float left      = 0.0f;
	float bottom    = 0.0f;   // baseline of the input line; the feed sits above
	float lineH     = 0.0f;
	float scaleX    = 0.0f;
	float scaleY    = 0.0f;
	float maxWidth  = 0.0f;   // text past this is cut
	float shadow    = 0.0f;
};

constexpr float FEED_LEFT_OF_WIDTH   = 0.03f;
constexpr float FEED_BOTTOM_OF_HEIGHT = 0.70f;
constexpr float FEED_WIDTH_OF_WIDTH   = 0.55f;
constexpr float FEED_SCALE_X          = 0.28f;   // per HUD unit (screen height / 448)
constexpr float FEED_SCALE_Y          = 0.50f;
constexpr float FEED_CELL_HEIGHT      = 20.0f;   // addresses.h, FONT_CELL_HEIGHT
constexpr float FEED_LINE_GAP         = 1.15f;

inline FeedLayout MeasureFeed(float screenW, float screenH) {
	FeedLayout l;
	const float unit = screenH / 448.0f;
	l.scaleX   = FEED_SCALE_X * unit;
	l.scaleY   = FEED_SCALE_Y * unit;
	l.lineH    = FEED_CELL_HEIGHT * l.scaleY * FEED_LINE_GAP;
	l.left     = screenW * FEED_LEFT_OF_WIDTH;
	l.bottom   = screenH * FEED_BOTTOM_OF_HEIGHT;
	l.maxWidth = screenW * FEED_WIDTH_OF_WIDTH;
	l.shadow   = l.lineH * 0.07f < 1.0f ? 1.0f : l.lineH * 0.07f;
	return l;
}

// A player whose snapshots have stopped for this long is marked away on the
// scoreboard. A few missed packets are not worth a word; a menu, a loading
// screen or a game left in the background is.
constexpr uint32_t QUIET_AFTER_MS = 3000;

// "  off 7.4 m" for a copy the server says is that far from the real thing,
// from DESYNC_SHOW_CM_ON_LIST; nothing under it or when nothing was compared.
constexpr uint16_t DESYNC_SHOW_CM_ON_LIST = 100;

inline void AppendDesync(char *raw, size_t cap, uint16_t offCm) {
	if (offCm == DESYNC_UNKNOWN || offCm < DESYNC_SHOW_CM_ON_LIST)
		return;
	const size_t n = std::strlen(raw);
	if (n >= cap)
		return;
	if (offCm >= DESYNC_MAX_CM)
		std::snprintf(raw + n, cap - n, "  off 655 m+");
	else if (offCm < 1000)
		std::snprintf(raw + n, cap - n, "  off %u.%u m", static_cast<unsigned>(offCm / 100),
		              static_cast<unsigned>(offCm % 100 / 10));
	else
		std::snprintf(raw + n, cap - n, "  off %u m", static_cast<unsigned>(offCm / 100));
}

// ---- the version mark --------------------------------------------------------
//
// "CoopIII 0.0.1", small and grey in the bottom-left corner, under the radar:
// which build is running, readable off any screenshot or video somebody sends
// with a bug in it.

constexpr const char *VERSION_MARK = "CoopIII " COOPIII_VERSION;

struct MarkLayout {
	float x      = 0.0f;
	float y      = 0.0f;   // top of the text
	float scaleX = 0.0f;
	float scaleY = 0.0f;
};

// The radar's lowest edge is RADAR_BOTTOM, 47 HUD units above the bottom of
// the screen, and its top is 76 more (re3 Radar.h, and what the retail
// draws); the mark sits in the strip under it.
constexpr float MARK_SCALE_X      = 0.22f;
constexpr float MARK_SCALE_Y      = 0.40f;
constexpr float MARK_MARGIN_UNITS = 3.0f;
constexpr float RADAR_BOTTOM_UNITS = 47.0f;
constexpr float RADAR_TOP_UNITS    = 47.0f + 76.0f;
// Scaled with the screen alone it was 10 px tall in a 960x540 window, which
// is no mark at all. Never smaller than this many pixels.
constexpr float MARK_MIN_TEXT_PX = 14.0f;
// CFont::PrintChar drops a glyph whose top is at or past SCREEN_WIDTH, on the
// y axis as well as the x one (addresses.h, CFont__PrintChar). A 958x1000
// window has its whole strip under the radar past that line, so the mark
// printed nothing there. It is kept this many pixels above it.
constexpr float MARK_CULL_CLEARANCE_PX = 2.0f;

inline MarkLayout MeasureVersionMark(float screenW, float screenH) {
	MarkLayout m;
	const float unit = screenH / 448.0f;
	float       grow = 1.0f;
	if (FEED_CELL_HEIGHT * MARK_SCALE_Y * unit < MARK_MIN_TEXT_PX)
		grow = MARK_MIN_TEXT_PX / (FEED_CELL_HEIGHT * MARK_SCALE_Y * unit);
	m.scaleX = MARK_SCALE_X * unit * grow;
	m.scaleY = MARK_SCALE_Y * unit * grow;
	m.x      = MARK_MARGIN_UNITS * unit;
	m.y      = screenH - FEED_CELL_HEIGHT * m.scaleY - MARK_MARGIN_UNITS * unit;

	const float lowest = screenW - MARK_CULL_CLEARANCE_PX;
	if (m.y > lowest) {
		// Still under the radar if the line is below its bottom edge, and over
		// the top of it otherwise, never across it.
		const float underRadar = screenH - RADAR_BOTTOM_UNITS * unit;
		const float overRadar  = screenH - RADAR_TOP_UNITS * unit - MARK_MARGIN_UNITS * unit -
		                         FEED_CELL_HEIGHT * m.scaleY;
		m.y = lowest >= underRadar ? lowest : (overRadar < lowest ? overRadar : lowest);
		if (m.y < 1.0f)
			m.y = 1.0f;
	}
	return m;
}

} // namespace coopiii
