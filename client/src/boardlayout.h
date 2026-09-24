// The scoreboard: everyone in the session on one panel, up while Tab is held
// or pinned with F9. What goes where and what each cell says, from the screen
// size and the roster alone; game/scoreboard.cpp draws it with
// CSprite2d::DrawRect and CFont, and tools/clienttest walks all of this.
//
//   +--------------------------------------------------------------+
//   | COOPIII                                                  2/8 |  title
//   |==============================================================|  accent
//   | server 127.0.0.1:2001                world hosted by noxx    |
//   | PLAYER              HEALTH   WANTED  STATUS          PING    |
//   | # noxx       HOST   [=====]  ]]]     DRIVING         |||| 38 |  you, lit
//   | # alice             [===  ]          ON FOOT         |||  95 |
//   |--------------------------------------------------------------|
//   | CoopIII 0.0.1                         out 1204  in 1188      |  footer
//   +--------------------------------------------------------------+
#pragma once

#include "chatfeed.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coopiii {

// ---- the grid ---------------------------------------------------------------
//
// Everything below is in board units. A unit is the HUD's own scale, value *
// SCREEN_WIDTH / 640 and value * SCREEN_HEIGHT / 448, except that the smaller
// of the two is used on both axes: a panel stretched with a wide screen is
// exactly what the widescreen fix exists to undo on the HUD, and taking the
// smaller one is also what keeps the whole thing on a tall window.

constexpr float BOARD_REF_WIDTH  = 640.0f;   // addresses.h, HUD_REF_WIDTH
constexpr float BOARD_REF_HEIGHT = 448.0f;   // addresses.h, HUD_REF_HEIGHT

constexpr float BOARD_WIDTH   = 360.0f;
constexpr float BOARD_PAD     = 6.0f;
constexpr float BOARD_TITLE_H = 24.0f;
constexpr float BOARD_ACCENT_H = 1.5f;
constexpr float BOARD_SUB_H   = 13.0f;
constexpr float BOARD_HEAD_H  = 12.0f;
constexpr float BOARD_ROW_H   = 17.0f;
constexpr float BOARD_GAP_H   = 3.0f;    // between the last row and the footer rule
constexpr float BOARD_FOOT_H  = 13.0f;   // per footer line

// Columns, from the panel's left edge.
constexpr float BOARD_CHIP_X   = 6.0f,   BOARD_CHIP_W   = 3.0f;
constexpr float BOARD_NICK_X   = 13.0f,  BOARD_NICK_W   = 104.0f;
constexpr float BOARD_HOST_X   = 120.0f, BOARD_HOST_W   = 24.0f;
constexpr float BOARD_HEALTH_X = 150.0f, BOARD_HEALTH_W = 52.0f;
constexpr float BOARD_STARS_X  = 208.0f, BOARD_STAR_STEP = 7.5f;
constexpr float BOARD_STATE_X  = 258.0f, BOARD_STATE_W  = 52.0f;
constexpr float BOARD_PING_X   = 314.0f;
constexpr float BOARD_PING_TEXT_X = 332.0f;

// The health bar and the armour bar under it, and the host badge.
constexpr float BOARD_HEALTH_BAR_H = 5.0f;
constexpr float BOARD_ARMOUR_BAR_H = 2.0f;
constexpr float BOARD_BAR_GAP      = 1.0f;
constexpr float BOARD_BADGE_H      = 9.0f;

// Four signal bars, bottom-aligned, each taller than the last.
constexpr int   BOARD_PING_BARS    = 4;
constexpr float BOARD_PING_BAR_W   = 2.5f;
constexpr float BOARD_PING_BAR_GAP = 1.5f;
constexpr float BOARD_PING_BAR_H0  = 3.0f;   // the first; each next is 2 taller

// CFont scales per unit. The title is FONT_HEADING, the face the HUD's money
// and clock are in; the rest is FONT_BANK, the subtitles' face. A glyph cell
// is FEED_CELL_HEIGHT * scaleY tall.
constexpr float BOARD_TITLE_SX = 0.50f, BOARD_TITLE_SY = 0.90f;
constexpr float BOARD_TEXT_SX  = 0.30f, BOARD_TEXT_SY  = 0.56f;
constexpr float BOARD_SMALL_SX = 0.24f, BOARD_SMALL_SY = 0.44f;
// The player count, beside the title in FONT_BANK.
constexpr float BOARD_COUNT_SX = 0.36f, BOARD_COUNT_SY = 0.66f;
// The wanted star is the HUD's own: ']' in FONT_HEADING (CHud::Draw at
// 0x00506B96). The HUD steps them 23 units apart at scale 0.8 x 1.35; this is
// the same shape at BOARD_STAR_STEP.
constexpr float BOARD_STAR_SX = 0.8f * BOARD_STAR_STEP / 23.0f;
constexpr float BOARD_STAR_SY = 1.35f * BOARD_STAR_STEP / 23.0f;

// The smallest text on the panel is never shorter than this while the window
// has room for it. Below it GTA's bank font stops being letters.
constexpr float BOARD_MIN_SMALL_PX = 11.0f;
// Kept clear all round, in pixels.
constexpr float BOARD_MARGIN_PX = 8.0f;
// Where the middle of the panel sits, as a fraction of the height it has:
// a little above centre, clear of the subtitles.
constexpr float BOARD_CENTRE_OF_HEIGHT = 0.45f;

struct BoardLayout {
	float unit   = 0.0f;   // pixels per board unit
	float left   = 0.0f;
	float top    = 0.0f;
	float width  = 0.0f;
	float height = 0.0f;
	int   rows   = 0;
	int   footLines = 1;

	// Tops of each band, in pixels.
	float titleTop = 0.0f;
	float accentTop = 0.0f;
	float subTop   = 0.0f;
	float headTop  = 0.0f;
	float rowsTop  = 0.0f;
	float footTop  = 0.0f;   // the rule; the first footer line is under it

	float X(float u) const { return left + u * unit; }
	float U(float u) const { return u * unit; }
	float RowTop(int i) const { return rowsTop + static_cast<float>(i) * BOARD_ROW_H * unit; }
	float FootLineTop(int i) const {
		return footTop + BOARD_ACCENT_H * unit + static_cast<float>(i) * BOARD_FOOT_H * unit;
	}
	// A line of text `scaleY` tall, centred in a band `bandH` units tall.
	float TextTop(float bandTop, float bandH, float scaleY) const {
		return bandTop + (bandH * unit - FEED_CELL_HEIGHT * scaleY * unit) * 0.5f;
	}
};

inline float BoardHeightUnits(int rows, int footLines) {
	return BOARD_TITLE_H + BOARD_ACCENT_H + BOARD_SUB_H + BOARD_HEAD_H +
	       static_cast<float>(rows) * BOARD_ROW_H + BOARD_GAP_H + BOARD_ACCENT_H +
	       static_cast<float>(footLines) * BOARD_FOOT_H + BOARD_PAD * 0.5f;
}

// CFont drops a glyph whose top is at or past SCREEN_WIDTH - on the y axis,
// not just the x one (CFont::PrintChar, addresses.h). So on a window taller
// than it is wide nothing prints below y = width, and the panel is kept above
// that line as well as on the screen.
inline float PrintableHeight(float screenW, float screenH) {
	return screenW < screenH ? screenW : screenH;
}

inline BoardLayout MeasureBoard(float screenW, float screenH, int rows, int footLines = 1) {
	BoardLayout b;
	if (rows < 1)
		rows = 1;
	if (rows > MAX_PLAYERS)
		rows = MAX_PLAYERS;
	if (footLines < 1)
		footLines = 1;
	if (footLines > 2)
		footLines = 2;
	b.rows      = rows;
	b.footLines = footLines;

	const float wantW = BOARD_WIDTH;
	const float wantH = BoardHeightUnits(rows, footLines);

	const float byW = screenW / BOARD_REF_WIDTH;
	const float byH = screenH / BOARD_REF_HEIGHT;
	float       unit = byW < byH ? byW : byH;

	const float minUnit = BOARD_MIN_SMALL_PX / (FEED_CELL_HEIGHT * BOARD_SMALL_SY);
	if (unit < minUnit)
		unit = minUnit;

	// Room wins over readability: a panel off the edge is read by nobody.
	const float usableH = PrintableHeight(screenW, screenH);
	const float roomW   = screenW - 2.0f * BOARD_MARGIN_PX;
	const float roomH   = usableH - 2.0f * BOARD_MARGIN_PX;
	const float fitW    = roomW / wantW;
	const float fitH    = roomH / wantH;
	const float fit     = fitW < fitH ? fitW : fitH;
	if (unit > fit)
		unit = fit;
	if (!(unit > 0.0f))
		unit = 0.0f;

	b.unit   = unit;
	b.width  = wantW * unit;
	b.height = wantH * unit;
	b.left   = (screenW - b.width) * 0.5f;
	b.top    = usableH * BOARD_CENTRE_OF_HEIGHT - b.height * 0.5f;
	if (b.top + b.height > usableH - BOARD_MARGIN_PX)
		b.top = usableH - BOARD_MARGIN_PX - b.height;
	if (b.top < BOARD_MARGIN_PX)
		b.top = BOARD_MARGIN_PX;

	b.titleTop  = b.top;
	b.accentTop = b.titleTop + BOARD_TITLE_H * unit;
	b.subTop    = b.accentTop + BOARD_ACCENT_H * unit;
	b.headTop   = b.subTop + BOARD_SUB_H * unit;
	b.rowsTop   = b.headTop + BOARD_HEAD_H * unit;
	b.footTop   = b.rowsTop + static_cast<float>(rows) * BOARD_ROW_H * unit + BOARD_GAP_H * unit;
	return b;
}

// ---- the rows ---------------------------------------------------------------

// Who goes in which row: you first, then everyone else in slot order. How
// many rows that is.
inline int BoardOrder(uint8_t localId, const bool (&active)[MAX_PLAYERS],
                      uint8_t (&out)[MAX_PLAYERS]) {
	int n = 0;
	if (localId < MAX_PLAYERS)
		out[n++] = localId;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (id != localId && active[id] && n < MAX_PLAYERS)
			out[n++] = id;
	return n;
}

enum class BoardState : uint8_t { OnFoot, Driving, Passenger, Wasted, Away };

// Quiet first: whatever else the last snapshot said is that old.
inline BoardState StateOf(bool dead, float health, bool seated, uint8_t seat,
                          uint32_t quietMs) {
	if (quietMs >= QUIET_AFTER_MS)
		return BoardState::Away;
	if (dead || !(health > 0.0f))
		return BoardState::Wasted;
	if (seated)
		return seat == 0 ? BoardState::Driving : BoardState::Passenger;
	return BoardState::OnFoot;
}

// What the status column says. "AWAY 12s" for somebody gone quiet, capped so
// it stays inside the column.
inline void StateLabel(char *out, size_t cap, BoardState s, uint32_t quietMs = 0) {
	if (cap == 0)
		return;
	switch (s) {
	case BoardState::OnFoot:    std::snprintf(out, cap, "ON FOOT");   return;
	case BoardState::Driving:   std::snprintf(out, cap, "DRIVING");   return;
	case BoardState::Passenger: std::snprintf(out, cap, "PASSENGER"); return;
	case BoardState::Wasted:    std::snprintf(out, cap, "WASTED");    return;
	case BoardState::Away: {
		const uint32_t sec = quietMs / 1000;
		if (sec > 999)
			std::snprintf(out, cap, "AWAY 999s+");
		else
			std::snprintf(out, cap, "AWAY %us", static_cast<unsigned>(sec));
		return;
	}
	}
	out[0] = '\0';
}

// How much of a bar is full: 0..1 of `full`, whatever a script or a cheat set.
inline float BarFill(float value, float full = 100.0f) {
	if (!(value > 0.0f) || !(full > 0.0f))
		return 0.0f;
	const float f = value / full;
	return f > 1.0f ? 1.0f : f;
}

inline uint8_t StarsLit(uint8_t stars) { return stars > 6 ? 6 : stars; }

// ---- the ping ---------------------------------------------------------------

enum class PingTone : uint8_t { None, Good, Ok, Bad };

struct PingSignal {
	uint8_t  bars = 0;   // lit, of BOARD_PING_BARS
	PingTone tone = PingTone::None;
};

constexpr uint16_t PING_GOOD_UNDER_MS = 100;
constexpr uint16_t PING_OK_UNDER_MS   = 200;

inline PingSignal SignalOf(uint16_t pingMs) {
	PingSignal s;
	if (pingMs == PING_NONE)
		return s;
	s.bars = pingMs < 60 ? 4 : pingMs < PING_GOOD_UNDER_MS ? 3 : pingMs < PING_OK_UNDER_MS ? 2 : 1;
	s.tone = pingMs < PING_GOOD_UNDER_MS ? PingTone::Good
	         : pingMs < PING_OK_UNDER_MS ? PingTone::Ok
	                                     : PingTone::Bad;
	return s;
}

// The number beside the bars, in milliseconds; nothing when it is not known,
// and past a second it says so rather than giving four digits.
inline void PingLabel(char *out, size_t cap, uint16_t pingMs) {
	if (cap == 0)
		return;
	if (pingMs == PING_NONE)
		out[0] = '\0';
	else if (pingMs >= 1000)
		std::snprintf(out, cap, "1s+");
	else
		std::snprintf(out, cap, "%u", static_cast<unsigned>(pingMs));
}

// ---- the title --------------------------------------------------------------

// "2/8": who is here, of how many the session holds.
inline void FormatCount(char *out, size_t cap, unsigned here, unsigned of = MAX_PLAYERS) {
	if (cap != 0)
		std::snprintf(out, cap, "%u/%u", here, of);
}

} // namespace coopiii
