// The chat feed, the line being typed, the scoreboard and the version mark:
// client/src/chatfeed.h and client/src/boardlayout.h.
// The drawing and the keys are game/chat.cpp and need the game; what they draw
// and what they send is all here.

#include "boardlayout.h"
#include "chatfeed.h"
#include "game/fontcull.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace coopiii;

namespace {

int g_chatFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_chatFailures;
}

std::string Copied(const char *in) {
	char out[FEED_MESSAGE];
	FeedCopy(out, sizeof out, in);
	return out;
}

std::string Typed(ChatLine &line) { return line.Text(); }

void TestWhatCFontIsHanded() {
	std::printf("\nwhat CFont is handed\n");
	Check(Copied("a~b~c") == "a-b-c", "a '~' would open a token that eats the line");
	Check(Copied("*star") == "+star", "a leading '*' would hide the whole line");
	Check(Copied("x*y") == "x*y", "one anywhere else is drawn as it is");
	Check(Copied("tab\there") == "tab?here", "control characters are not in the width table");
	Check(Copied("caf\xE9") == "caf?", "and neither is anything past '~'");
	Check(Copied("MiXeD") == "MiXeD", "the case is left alone, unlike a nametag");

	char small[4];
	FeedCopy(small, sizeof small, "abcdef");
	Check(std::strcmp(small, "abc") == 0, "cut to what fits, and terminated");
	FeedCopy(small, sizeof small, nullptr);
	Check(small[0] == '\0', "nothing is an empty line");
}

void TestWhatAKeyboardTypes() {
	std::printf("\nwhat a keyboard types\n");
	Check(ChatGlyph('a') == 'a' && ChatGlyph(' ') == ' ' && ChatGlyph('}') == '}',
	      "plain ASCII is itself");
	Check(ChatGlyph('~') == '-', "a '~' is never typed into a token");
	Check(ChatGlyph(0xF1) == 'n' && ChatGlyph(0xD1) == 'N', "an n with a tilde is an n");
	Check(ChatGlyph(0xE1) == 'a' && ChatGlyph(0xE9) == 'e' && ChatGlyph(0xED) == 'i' &&
	          ChatGlyph(0xF3) == 'o' && ChatGlyph(0xFA) == 'u' && ChatGlyph(0xFC) == 'u',
	      "the accented vowels are their vowels");
	Check(ChatGlyph(0xC7) == 'C' && ChatGlyph(0xE7) == 'c' && ChatGlyph(0xDF) == 's',
	      "and so on through the rest of Latin-1");
	Check(ChatGlyph(0xBF) == '?' && ChatGlyph(0xA1) == '!', "the upside-down marks too");
	Check(ChatGlyph(0x201C) == '"' && ChatGlyph(0x2019) == '\'' && ChatGlyph(0x2014) == '-',
	      "the quotes and dashes a paste brings are the plain ones");
	Check(ChatGlyph(0x0A) == 0 && ChatGlyph(0x7F) == 0 && ChatGlyph(0x4E2D) == 0 &&
	          ChatGlyph(0xD83D) == 0,
	      "control characters, other scripts and half an emoji are refused");
}

void TestTheLines() {
	std::printf("\nthe lines\n");
	char   line[FEED_MESSAGE];
	size_t nickLen = 0;
	FormatChatLine(line, sizeof line, "alice", "hello there", &nickLen);
	Check(std::strcmp(line, "alice: hello there") == 0, "who said it, then what");
	Check(nickLen == 6, "and how much of it is the name, colon included");

	FormatJoined(line, sizeof line, "bob");
	Check(std::strcmp(line, "bob joined") == 0, "somebody arriving");
	FormatLeft(line, sizeof line, "bob", LEAVE_QUIT);
	Check(std::strcmp(line, "bob left") == 0, "somebody quitting");
	FormatLeft(line, sizeof line, "bob", LEAVE_TIMEOUT);
	Check(std::strcmp(line, "bob lost connection") == 0, "somebody timing out");
	FormatLeft(line, sizeof line, "bob", LEAVE_KICKED);
	Check(std::strcmp(line, "bob was kicked") == 0, "somebody thrown out");

	const std::string longNick(100, 'n');
	const std::string longText(400, 't');
	FormatChatLine(line, sizeof line, longNick.c_str(), longText.c_str(), &nickLen);
	const char *colon = std::strchr(line, ':');
	Check(colon != nullptr && static_cast<size_t>(colon - line) == NICK_LEN - 1 &&
	          nickLen == NICK_LEN,
	      "a nick longer than the wire's is cut to it");
	Check(std::strlen(colon + 2) == CHAT_LEN - 1, "and so is the text");

	FormatChatLine(line, sizeof line, "~eve", "*hi");
	Check(std::strcmp(line, "-eve: *hi") == 0,
	      "a nick cannot carry a token in, and the text is not the line's start");
	FormatChatLine(line, sizeof line, "", "hi", &nickLen);
	Check(std::strcmp(line, "?: hi") == 0 && nickLen == 2, "no nick is a question mark");
}

void TestALongMessageWraps() {
	std::printf("\na long message goes on as many lines as it takes\n");
	ChatFeed feed;
	feed.Push(FeedKind::Chat, "alice: short", 5, 2, 6);
	Check(feed.Count() == 1 && feed.Line(0).playerId == 2 && feed.Line(0).nickLen == 6,
	      "a short one is one line, with its name");

	std::string words = "bob:";
	while (words.size() < 150)
		words += " word";
	feed.Clear();
	feed.Push(FeedKind::Chat, words.c_str(), 5, 1, 4);
	Check(feed.Count() == 3, "150 characters are three lines");
	bool fits = true, wordsWhole = true;
	for (size_t i = 0; i < feed.Count(); ++i) {
		const std::string t = feed.Line(i).text;
		if (t.size() > FEED_WRAP)
			fits = false;
		const size_t end = t.find_last_not_of(' ');
		if (end != std::string::npos && t.compare(end - 3, 4, "word") != 0)
			wordsWhole = false;
	}
	Check(fits, "none longer than FEED_WRAP");
	Check(wordsWhole, "broken between words, not through one");
	Check(std::string(feed.Line(1).text).compare(0, 2, FEED_CONTINUED) == 0 &&
	          feed.Line(1).nickLen == 0 && feed.Line(1).playerId == INVALID_PLAYER,
	      "the rest indented, and without the name's colour");
	Check(feed.Line(0).nickLen == 4 && feed.Line(0).playerId == 1, "the first keeps it");

	feed.Clear();
	const std::string oneWord(130, 'x');
	feed.Push(FeedKind::Chat, oneWord.c_str(), 5);
	size_t total = 0;
	for (size_t i = 0; i < feed.Count(); ++i)
		total += std::strlen(feed.Line(i).text) - (i ? 2 : 0);
	Check(feed.Count() == 3 && total == 130, "a word longer than a line is split, losing nothing");
}

void TestTheFeedKeepsTheLast() {
	std::printf("\nthe feed keeps the last few\n");
	ChatFeed feed;
	Check(feed.Count() == 0, "starts empty");
	char text[16];
	for (size_t i = 0; i < FEED_LINES + 3; ++i) {
		std::snprintf(text, sizeof text, "line %u", static_cast<unsigned>(i));
		feed.Push(i % 2 ? FeedKind::Chat : FeedKind::Notice, text, 1000u + static_cast<uint32_t>(i));
	}
	Check(feed.Count() == FEED_LINES, "never more than it shows");
	Check(std::strcmp(feed.Line(0).text, "line 3") == 0 && feed.Line(0).atMs == 1003,
	      "the oldest held is the one after the ones pushed out");
	Check(std::strcmp(feed.Line(FEED_LINES - 1).text, "line 12") == 0 &&
	          feed.Line(FEED_LINES - 1).kind == FeedKind::Notice,
	      "the newest is last, with its kind");
	feed.Clear();
	Check(feed.Count() == 0, "and a new session starts it empty");
}

void TestTheFade() {
	std::printf("\nhow long a line stays up\n");
	Check(FeedAlpha(0, false) == 255, "new: solid");
	Check(FeedAlpha(FEED_SHOW_MS - FEED_FADE_MS, false) == 255, "solid until the fade starts");
	const uint8_t half = FeedAlpha(FEED_SHOW_MS - FEED_FADE_MS / 2, false);
	Check(half > 100 && half < 155, "half way through the fade, about half");
	Check(FeedAlpha(FEED_SHOW_MS, false) == 0, "gone once its time is up");
	Check(FeedAlpha(FEED_SHOW_MS * 50, true) == 255, "every line held is shown while typing");
	const uint32_t atMs  = 0xFFFFFF00u;
	const uint32_t nowMs = atMs + 500u;   // the clock wrapped in between
	Check(FeedAlpha(nowMs - atMs, false) == 255, "a clock that wrapped reads as a new line");
}

void TestTheLineBeingTyped() {
	std::printf("\nthe line being typed\n");
	ChatLine line;
	Check(!line.Type('a'), "nothing is taken before the line is open");
	line.Begin();
	Check(line.Open() && line.Length() == 0 && line.Caret() == 0, "opens empty");
	line.Type('h');
	line.Type('i');
	Check(!line.Type('\n') && !line.Type(0x7F) && !line.Type(0x4E2D),
	      "only what CFont and the wire can both carry");
	Check(Typed(line) == "hi" && line.Caret() == 2, "what was typed, the caret after it");

	line.Type(0xF1);
	Check(Typed(line) == "hin", "an accented letter goes in as its letter");

	line.Left();
	line.Left();
	line.Type('X');
	Check(Typed(line) == "hXin" && line.Caret() == 2, "typed at the caret, not at the end");
	line.Home();
	line.Type('>');
	line.End();
	line.Type('<');
	Check(Typed(line) == ">hXin<", "Home and End");
	line.Home();
	line.Right();
	line.Right();
	line.Backspace();
	Check(Typed(line) == ">Xin<" && line.Caret() == 1, "backspace takes what is before the caret");
	line.Delete();
	Check(Typed(line) == ">in<" && line.Caret() == 1, "delete what is after it");
	line.End();
	line.Delete();
	line.Home();
	line.Backspace();
	line.Left();
	Check(Typed(line) == ">in<" && line.Caret() == 0, "and neither runs off either end");
	line.Clear();
	Check(Typed(line) == "" && line.Caret() == 0, "shift-delete empties it");

	for (size_t i = 0; i < CHAT_LEN + 10; ++i)
		line.Type('x');
	Check(line.Length() == CHAT_LEN - 1, "never more than the packet carries");
	line.Home();
	Check(!line.Type('y') && line.Length() == CHAT_LEN - 1, "not even in the middle of it");

	char out[CHAT_LEN];
	Check(line.Submit(out) && std::strlen(out) == CHAT_LEN - 1, "a full line goes out whole");
	Check(!line.Open(), "and sending closes it");

	line.Begin();
	line.Type(' ');
	line.Type(' ');
	std::memset(out, 'z', sizeof out);
	Check(!line.Submit(out) && out[0] == 'z', "a line of spaces is not sent");
	Check(!line.Open(), "but it is closed all the same");

	line.Begin();
	line.Type('o');
	line.Cancel();
	Check(!line.Open() && line.Length() == 0, "escape drops what was typed");
}

void TestAPaste() {
	std::printf("\na paste\n");
	ChatLine line;
	line.Begin();
	line.Type('[');
	line.Type(']');
	line.Left();
	const size_t n = line.Paste(L"one\r\ntwo\tthree \x201Cq\x201D");
	Check(Typed(line) == "[one two three \"q\"]", "at the caret, line breaks and tabs as spaces");
	Check(n == 17 && line.Caret() == 18, "every character counted, the caret after the last");

	const std::wstring big(300, L'p');
	line.Paste(big.c_str());
	Check(line.Length() == CHAT_LEN - 1, "and a long one stops where the packet does");

	ChatLine closed;
	Check(closed.Paste(L"x") == 0, "nothing goes in a line that is not open");
}

void TestUpAndDown() {
	std::printf("\nup and down go through what was sent\n");
	ChatLine line;
	char     out[CHAT_LEN];
	for (const char *s : {"first", "second", "second", "third"}) {
		line.Begin();
		line.Paste(s);
		line.Submit(out);
	}
	Check(line.HistoryCount() == 3, "the same line twice running is remembered once");

	line.Begin();
	line.Paste("draft");
	line.Older();
	Check(Typed(line) == "third" && line.Caret() == 5, "up is the last one sent, caret at its end");
	line.Older();
	line.Older();
	line.Older();
	Check(Typed(line) == "first", "and stops at the oldest");
	line.Newer();
	Check(Typed(line) == "second", "down comes back");
	line.Newer();
	line.Newer();
	Check(Typed(line) == "draft", "to what was being typed before the first up");
	line.Newer();
	Check(Typed(line) == "draft", "and no further");

	for (int i = 0; i < 20; ++i) {
		line.Begin();
		char s[8];
		std::snprintf(s, sizeof s, "m%d", i);
		line.Paste(s);
		line.Submit(out);
	}
	Check(line.HistoryCount() == CHAT_HISTORY, "only the last few are kept");
	line.Begin();
	for (size_t i = 0; i < CHAT_HISTORY; ++i)
		line.Older();
	Check(Typed(line) == "m10", "and the oldest of those is the tenth from last");
}

void TestWhereItGoes() {
	std::printf("\nwhere it goes\n");
	const FeedLayout base = MeasureFeed(640.0f, 448.0f);
	Check(base.scaleX == FEED_SCALE_X && base.scaleY == FEED_SCALE_Y,
	      "at the HUD's own grid, the scale as written");
	Check(base.lineH > FEED_CELL_HEIGHT * FEED_SCALE_Y, "lines do not touch");
	Check(base.bottom < 448.0f && base.left > 0.0f, "on the screen");
	Check(base.bottom - (FEED_LINES + 1) * base.lineH > 0.0f, "and all of it, not just the bottom");

	const FeedLayout tall = MeasureFeed(1920.0f, 1080.0f);
	const FeedLayout wide = MeasureFeed(2560.0f, 1080.0f);
	Check(tall.scaleY == wide.scaleY && tall.lineH == wide.lineH,
	      "the width of the screen does not change the size of the text");
	Check(wide.maxWidth > tall.maxWidth, "only how much of a line fits");
	Check(tall.shadow >= 1.0f && base.shadow >= 1.0f, "the shadow is always a pixel at least");

	// A FEED_WRAP line of the widest glyphs, a whole cell each (32 * scaleX),
	// against the room it has on the narrowest screen anybody plays on.
	const FeedLayout square = MeasureFeed(640.0f, 480.0f);
	Check(FEED_WRAP * 32.0f * square.scaleX * 0.6f < square.maxWidth,
	      "a wrapped line of ordinary text fits across a 4:3 screen");
}

void TestTheColours() {
	std::printf("\nwhose line is whose\n");
	bool distinct = true;
	for (uint8_t a = 0; a < MAX_PLAYERS; ++a)
		for (uint8_t b = a + 1; b < MAX_PLAYERS; ++b) {
			const NickColour x = ChatNickColour(a), y = ChatNickColour(b);
			if (x.r == y.r && x.g == y.g && x.b == y.b)
				distinct = false;
		}
	Check(distinct, "every slot has a colour of its own");
	const NickColour none = ChatNickColour(INVALID_PLAYER);
	Check(none.r == none.g && none.g == none.b, "and nobody's is a plain grey");
}

// With PrintChar's y test as retail has it, unless `fixed`: everything these
// tests ask was written against the retail cull first.
BoardLayout Board(float screenW, float screenH, int rows, int footLines = 1, bool fixed = false) {
	return MeasureBoard(screenW, screenH, rows, footLines,
	                    game::TextCullLine(screenW, screenH, fixed));
}

MarkLayout Mark(float screenW, float screenH, bool fixed = false) {
	return MeasureVersionMark(screenH, game::TextCullLine(screenW, screenH, fixed));
}

bool Inside(const BoardLayout &b, float screenW, float screenH, bool fixed = false) {
	return b.left >= 0.0f && b.top >= 0.0f && b.left + b.width <= screenW &&
	       b.top + b.height <= PrintableHeight(screenH, game::TextCullLine(screenW, screenH, fixed));
}

void TestWhereTheScoreboardGoes() {
	std::printf("\nwhere the scoreboard goes\n");

	const BoardLayout full = Board(1920.0f, 1080.0f, MAX_PLAYERS);
	Check(Inside(full, 1920.0f, 1080.0f), "a full session fits a 1080p screen");
	Check(std::fabs(full.unit - 1080.0f / BOARD_REF_HEIGHT) < 1e-4f,
	      "at the HUD's own scale, by height on a wide screen");
	Check(std::fabs(full.left + full.width * 0.5f - 960.0f) < 0.01f, "centred across");
	Check(full.top + full.height * 0.5f < 540.0f, "and a little above the middle, clear of subtitles");

	const BoardLayout wider = Board(2560.0f, 1080.0f, MAX_PLAYERS);
	Check(wider.unit == full.unit && wider.width == full.width,
	      "a wider screen does not stretch it");

	const BoardLayout four3 = Board(640.0f, 480.0f, 2);
	Check(four3.unit > 1.0f &&
	          FEED_CELL_HEIGHT * BOARD_SMALL_SY * four3.unit >= BOARD_MIN_SMALL_PX - 0.01f,
	      "640x480 raises it until the smallest text is readable");
	Check(Inside(four3, 640.0f, 480.0f), "and it still fits");
	const BoardLayout four3Full = Board(640.0f, 480.0f, MAX_PLAYERS);
	Check(Inside(four3Full, 640.0f, 480.0f), "a full session too");

	// The window the version mark went missing in: taller than it is wide.
	const BoardLayout tall = Board(958.0f, 1000.0f, MAX_PLAYERS, 2);
	Check(std::fabs(tall.unit - 958.0f / BOARD_REF_WIDTH) < 1e-4f, "a tall window scales it by width");
	Check(Inside(tall, 958.0f, 1000.0f) && tall.top + tall.height < 958.0f,
	      "and keeps every glyph above y = width, where CFont stops printing");
	// With PrintChar's y test reading the height, the line is the bottom.
	const BoardLayout tallFixed = Board(958.0f, 1000.0f, MAX_PLAYERS, 2, true);
	Check(tallFixed.unit == tall.unit && Inside(tallFixed, 958.0f, 1000.0f, true) &&
	          std::fabs(tallFixed.top + tallFixed.height * 0.5f - 1000.0f * BOARD_CENTRE_OF_HEIGHT) <
	              0.01f,
	      "fixed, the same panel is placed on the whole height instead");
	const BoardLayout thin      = Board(400.0f, 1000.0f, MAX_PLAYERS, 2);
	const BoardLayout thinFixed = Board(400.0f, 1000.0f, MAX_PLAYERS, 2, true);
	Check(thin.top + thin.height < 400.0f && thinFixed.top + thinFixed.height > 400.0f &&
	          Inside(thinFixed, 400.0f, 1000.0f, true),
	      "and only then does a narrow window get it across y = width");

	const BoardLayout tiny = Board(320.0f, 240.0f, MAX_PLAYERS, 2);
	Check(tiny.unit > 0.0f && Inside(tiny, 320.0f, 240.0f),
	      "a window too small for readable text gets it small rather than off the edge");

	const BoardLayout none = Board(1280.0f, 720.0f, 0, 0);
	Check(none.rows == 1 && none.footLines == 1, "never fewer than one row and one footer line");
	const BoardLayout lots = Board(1280.0f, 720.0f, 40, 9);
	Check(lots.rows == MAX_PLAYERS && lots.footLines == 2, "nor more than the session and two lines");

	const BoardLayout two = Board(1280.0f, 720.0f, 2);
	const BoardLayout three = Board(1280.0f, 720.0f, 3);
	Check(std::fabs(three.height - two.height - BOARD_ROW_H * two.unit) < 0.01f,
	      "each player is one row taller");
}

void TestTheScoreboardBands() {
	std::printf("\nthe scoreboard, band by band\n");
	const BoardLayout b = Board(1600.0f, 900.0f, 5, 2);
	Check(b.titleTop == b.top && b.titleTop < b.accentTop && b.accentTop < b.subTop &&
	          b.subTop < b.headTop && b.headTop < b.rowsTop && b.rowsTop < b.footTop,
	      "title, accent, session line, headings, rows, footer, top to bottom");
	Check(b.RowTop(4) + b.U(BOARD_ROW_H) <= b.footTop, "the last row ends above the footer rule");
	Check(b.FootLineTop(1) + b.U(BOARD_FOOT_H) <= b.top + b.height + 0.01f,
	      "and the second footer line inside the panel");

	const float rowTop = b.RowTop(2);
	const float textY  = b.TextTop(rowTop, BOARD_ROW_H, BOARD_TEXT_SY);
	Check(std::fabs((textY - rowTop) * 2.0f + FEED_CELL_HEIGHT * BOARD_TEXT_SY * b.unit -
	                BOARD_ROW_H * b.unit) < 0.01f,
	      "row text is centred in its row");

	Check(FEED_CELL_HEIGHT * BOARD_TITLE_SY <= BOARD_TITLE_H &&
	          FEED_CELL_HEIGHT * BOARD_TEXT_SY <= BOARD_ROW_H &&
	          FEED_CELL_HEIGHT * BOARD_SMALL_SY <= BOARD_HEAD_H &&
	          FEED_CELL_HEIGHT * BOARD_SMALL_SY <= BOARD_FOOT_H &&
	          FEED_CELL_HEIGHT * BOARD_STAR_SY <= BOARD_ROW_H,
	      "every line of text is no taller than its band");
	Check(BOARD_HEALTH_BAR_H + BOARD_BAR_GAP + BOARD_ARMOUR_BAR_H < BOARD_ROW_H &&
	          BOARD_PING_BAR_H0 + 2.0f * (BOARD_PING_BARS - 1) < BOARD_ROW_H,
	      "the bars fit in a row");

	Check(BOARD_CHIP_X + BOARD_CHIP_W < BOARD_NICK_X && BOARD_NICK_X + BOARD_NICK_W <= BOARD_HOST_X &&
	          BOARD_HOST_X + BOARD_HOST_W < BOARD_HEALTH_X &&
	          BOARD_HEALTH_X + BOARD_HEALTH_W < BOARD_STARS_X &&
	          BOARD_STARS_X + 6 * BOARD_STAR_STEP <= BOARD_STATE_X &&
	          BOARD_STATE_X + BOARD_STATE_W < BOARD_PING_X,
	      "the columns do not overlap");
	const float barsEnd = BOARD_PING_X + BOARD_PING_BARS * (BOARD_PING_BAR_W + BOARD_PING_BAR_GAP);
	Check(barsEnd <= BOARD_PING_TEXT_X && BOARD_PING_TEXT_X < BOARD_WIDTH - BOARD_PAD,
	      "the ping number sits after its bars and inside the panel");
}

void TestTheScoreboardRows() {
	std::printf("\nthe scoreboard, row by row\n");
	bool    active[MAX_PLAYERS] = {};
	uint8_t order[MAX_PLAYERS];
	active[1] = active[4] = active[6] = true;
	int n = BoardOrder(4, active, order);
	Check(n == 3 && order[0] == 4 && order[1] == 1 && order[2] == 6,
	      "you first, then everybody else in slot order");
	active[4] = false;
	n = BoardOrder(4, active, order);
	Check(n == 3 && order[0] == 4, "you whether the roster lists you or not");
	n = BoardOrder(INVALID_PLAYER, active, order);
	Check(n == 2 && order[0] == 1 && order[1] == 6, "and only the others before you have a slot");
	for (bool &a : active)
		a = true;
	n = BoardOrder(0, active, order);
	Check(n == MAX_PLAYERS, "a full session is a full board, you counted once");

	Check(StateOf(false, 80.0f, false, 0, 0) == BoardState::OnFoot, "on foot");
	Check(StateOf(false, 80.0f, true, 0, 0) == BoardState::Driving, "the driver's seat is driving");
	Check(StateOf(false, 80.0f, true, 2, 0) == BoardState::Passenger, "any other is a passenger");
	Check(StateOf(true, 80.0f, true, 0, 0) == BoardState::Wasted &&
	          StateOf(false, 0.0f, false, 0, 0) == BoardState::Wasted,
	      "dead, or out of health, is wasted");
	Check(StateOf(true, 0.0f, true, 0, QUIET_AFTER_MS) == BoardState::Away,
	      "somebody gone quiet is away, whatever the last snapshot said");
	Check(StateOf(false, 80.0f, false, 0, QUIET_AFTER_MS - 1) == BoardState::OnFoot,
	      "a couple of missed packets are not");

	char s[24];
	StateLabel(s, sizeof s, BoardState::Passenger);
	Check(std::strcmp(s, "PASSENGER") == 0, "the labels are the HUD's capitals");
	StateLabel(s, sizeof s, BoardState::Away, 12500);
	Check(std::strcmp(s, "AWAY 12s") == 0, "away says for how long");
	StateLabel(s, sizeof s, BoardState::Away, 5000000);
	Check(std::strcmp(s, "AWAY 999s+") == 0, "without running out of its column");

	Check(BarFill(50.0f) == 0.5f && BarFill(250.0f) == 1.0f && BarFill(-4.0f) == 0.0f &&
	          BarFill(std::nanf("")) == 0.0f,
	      "a bar is never more than full nor less than empty");
	Check(StarsLit(9) == 6 && StarsLit(3) == 3, "six stars is the most");

	char count[8];
	FormatCount(count, sizeof count, 2);
	Check(std::strcmp(count, "2/8") == 0, "the title counts who is here of how many");
}

void TestThePingBars() {
	std::printf("\nthe ping bars\n");
	const PingSignal none = SignalOf(PING_NONE);
	Check(none.bars == 0 && none.tone == PingTone::None, "nothing lit while the server has not said");
	const PingSignal fast = SignalOf(38);
	Check(fast.bars == 4 && fast.tone == PingTone::Good, "a LAN ping is four green bars");
	const PingSignal fine = SignalOf(95);
	Check(fine.bars == 3 && fine.tone == PingTone::Good, "under a tenth of a second is still good");
	const PingSignal ok = SignalOf(PING_GOOD_UNDER_MS);
	Check(ok.bars == 2 && ok.tone == PingTone::Ok, "from there it is only ok");
	const PingSignal bad = SignalOf(PING_OK_UNDER_MS);
	Check(bad.bars == 1 && bad.tone == PingTone::Bad, "and from twice that it is bad");

	char s[8];
	PingLabel(s, sizeof s, 38);
	Check(std::strcmp(s, "38") == 0, "the number beside it");
	PingLabel(s, sizeof s, 1400);
	Check(std::strcmp(s, "1s+") == 0, "a second or more says so");
	PingLabel(s, sizeof s, PING_NONE);
	Check(s[0] == '\0', "and none is blank");
}

void TestTheCommands() {
	std::printf("\ncommands in the chat line\n");
	Check(ParseChatCommand("hello /kick 2").kind == ChatCommand::None,
	      "a line that does not start with a slash is chat");
	const ParsedCommand kick = ParseChatCommand("/kick 2");
	Check(kick.kind == ChatCommand::Kick && std::strcmp(kick.arg, "2") == 0, "/kick and its number");
	const ParsedCommand loud = ParseChatCommand("/KICK   bob  ");
	Check(loud.kind == ChatCommand::Kick && std::strcmp(loud.arg, "bob") == 0,
	      "in any case, and the spaces round the name go");
	const ParsedCommand bare = ParseChatCommand("/kick");
	Check(bare.kind == ChatCommand::Kick && bare.arg[0] == '\0', "/kick alone names nobody");
	const ParsedCommand longer = ParseChatCommand("/kickall now");
	Check(longer.kind == ChatCommand::Unknown && std::strcmp(longer.word, "kickall") == 0,
	      "a longer word is not /kick");
	const ParsedCommand slash = ParseChatCommand("/");
	Check(slash.kind == ChatCommand::Unknown && slash.word[0] == '\0',
	      "and a slash alone is not a command either");

	const RosterEntry roster[] = {{0, "alice"}, {1, "bob"}, {3, "Bobby"}, {5, "carol"}};
	uint8_t id = 0;
	Check(ResolveKickTarget("2", roster, 4, &id) == KickTarget::Found && id == 1,
	      "2 is slot 1, the number the list shows");
	Check(ResolveKickTarget("4", roster, 4, &id) == KickTarget::Found && id == 3, "4 is slot 3");
	Check(ResolveKickTarget("3", roster, 4, &id) == KickTarget::Nobody && id == INVALID_PLAYER,
	      "an empty slot's number is nobody");
	Check(ResolveKickTarget("0", roster, 4, &id) == KickTarget::Nobody, "there is no number 0");
	Check(ResolveKickTarget("99999999999", roster, 4, &id) == KickTarget::Nobody,
	      "and a huge number is nobody too");
	Check(ResolveKickTarget("BOB", roster, 4, &id) == KickTarget::Found && id == 1,
	      "a whole name in any case, although another name starts with it");
	Check(ResolveKickTarget("bobb", roster, 4, &id) == KickTarget::Found && id == 3,
	      "the start of exactly one name");
	Check(ResolveKickTarget("bo", roster, 4, &id) == KickTarget::SeveralNames &&
	          id == INVALID_PLAYER,
	      "the start of two names is neither of them");
	Check(ResolveKickTarget("dave", roster, 4, &id) == KickTarget::Nobody, "a name nobody has");
	Check(ResolveKickTarget("", roster, 4, &id) == KickTarget::Nobody, "and nothing names nobody");
	const RosterEntry numbers[] = {{0, "alice"}, {4, "1234"}};
	Check(ResolveKickTarget("1234", numbers, 2, &id) == KickTarget::Found && id == 4,
	      "a number nobody has is somebody's name");
	Check(ResolveKickTarget("5", numbers, 2, &id) == KickTarget::Found && id == 4,
	      "though the number the list shows still wins");
}

void TestTheVersionMark() {
	std::printf("\nthe version mark\n");
	Check(std::string(VERSION_MARK) == std::string("CoopIII ") + COOPIII_VERSION,
	      "it says CoopIII and the version");
	Check(Copied(VERSION_MARK) == VERSION_MARK, "and CFont can draw every character of it");
	for (const float h : {448.0f, 480.0f, 720.0f, 1080.0f, 2160.0f}) {
		const MarkLayout m = Mark(h * 16.0f / 9.0f, h);
		const float unit   = h / 448.0f;
		const float bottom = m.y + FEED_CELL_HEIGHT * m.scaleY;
		if (!(m.x > 0.0f && bottom < h && m.y > h - RADAR_BOTTOM_UNITS * unit)) {
			Check(false, "in the strip under the radar, at every height");
			return;
		}
	}
	Check(true, "in the strip under the radar, at every height");
	const MarkLayout a = Mark(1280.0f, 720.0f);
	const MarkLayout b = Mark(1920.0f, 720.0f);
	Check(a.scaleY == b.scaleY && a.x == b.x && a.y == b.y, "and a wider screen does not move it");
	const MarkLayout small = Mark(960.0f, 540.0f);
	Check(FEED_CELL_HEIGHT * small.scaleY >= MARK_MIN_TEXT_PX - 0.01f &&
	          small.scaleX / small.scaleY == MARK_SCALE_X / MARK_SCALE_Y,
	      "a 960x540 window still gets it at a readable size, in proportion");
	const MarkLayout big = Mark(1920.0f, 1080.0f);
	Check(std::fabs(big.scaleY - MARK_SCALE_Y * (1080.0f / 448.0f)) < 1e-5f,
	      "and a big screen is left as it was");
	Check(big.y == 1080.0f - FEED_CELL_HEIGHT * big.scaleY - MARK_MARGIN_UNITS * (1080.0f / 448.0f),
	      "at the very bottom, where it always was, on a screen wider than it is tall");

	// Retail CFont::PrintChar drops a glyph whose top is at y >= SCREEN_WIDTH.
	const MarkLayout tall = Mark(958.0f, 1000.0f);
	const float      tallUnit = 1000.0f / 448.0f;
	Check(tall.y < 958.0f && tall.y > 0.0f, "a 958x1000 window gets it above y = width");
	Check(tall.y >= 1000.0f - RADAR_BOTTOM_UNITS * tallUnit &&
	          tall.y + FEED_CELL_HEIGHT * tall.scaleY < 1000.0f,
	      "and still in the strip under the radar, all of it on the screen");
	const MarkLayout narrow = Mark(600.0f, 1000.0f);
	const float      radarTop = 1000.0f - RADAR_TOP_UNITS * tallUnit;
	const float      radarBot = 1000.0f - RADAR_BOTTOM_UNITS * tallUnit;
	Check(narrow.y < 600.0f &&
	          (narrow.y + FEED_CELL_HEIGHT * narrow.scaleY <= radarTop || narrow.y >= radarBot),
	      "a narrower one moves it above the radar, never across it");
	const MarkLayout square = Mark(1000.0f, 1000.0f);
	Check(square.y < 1000.0f, "and a square one keeps it above the line too");

	// PrintChar's y test fixed: the line is the bottom of the screen, and the
	// mark goes back where a wide screen of that height has it.
	const MarkLayout wideSame = Mark(1920.0f, 1000.0f);
	bool             home     = true;
	for (const float w : {958.0f, 600.0f, 1000.0f}) {
		const MarkLayout m = Mark(w, 1000.0f, true);
		home = home && m.x == wideSame.x && m.y == wideSame.y && m.scaleY == wideSame.scaleY;
	}
	Check(home, "fixed, a tall window gets it at the very bottom, as a wide one does");
	Check(wideSame.y > 958.0f, "which is below y = width in a 958x1000 one");
}

void TestTheChatLineInATallWindow() {
	std::printf("\nthe chat line in a tall window\n");
	// game/chat.cpp lifts the line by this much before drawing the feed.
	const auto lifted = [](float w, float h, bool fixed) {
		FeedLayout l = MeasureFeed(w, h);
		l.bottom -= game::LiftAboveCull(l.bottom + l.shadow, game::TextCullLine(w, h, fixed), h);
		return l;
	};
	bool still = true;
	for (const float h : {448.0f, 720.0f, 1080.0f, 2160.0f}) {
		const FeedLayout a = MeasureFeed(h * 16.0f / 9.0f, h);
		const FeedLayout b = lifted(h * 16.0f / 9.0f, h, false);
		const FeedLayout c = lifted(h * 4.0f / 3.0f, h, false);
		still = still && a.bottom == b.bottom && c.bottom == MeasureFeed(h * 4.0f / 3.0f, h).bottom;
	}
	Check(still, "a wide screen does not move it");
	Check(lifted(958.0f, 1000.0f, false).bottom == MeasureFeed(958.0f, 1000.0f).bottom,
	      "nor a 958x1000 window, where 70% down is still above y = width");

	const FeedLayout narrow = MeasureFeed(600.0f, 1000.0f);
	Check(narrow.bottom >= 600.0f, "a 600x1000 one puts the line past y = width");
	const FeedLayout up = lifted(600.0f, 1000.0f, false);
	Check(up.bottom + up.shadow < 600.0f && up.bottom + up.shadow > 590.0f,
	      "so with the retail test it goes up to just above it, shadow and all");
	Check(lifted(600.0f, 1000.0f, true).bottom == narrow.bottom,
	      "and with the test fixed it stays where it was");
}

// The owner's run on 2026-09-24, two 958x1000 windows side by side: the text is
// sized by the height and the room is 55% of the width, and a 46-character
// notice ran off the edge of it.
void TestANoticeIsBrokenWhereTheFontSaysSo() {
	std::printf("\na line is broken where the font says it no longer fits\n");
	const FeedLayout l    = MeasureFeed(958.0f, 1000.0f);
	const float      glyph = 12.0f;   // a wide face's letters at this scale, near enough
	const char      *line  = "noxx3 is on Give Me Liberty, and you are in it";
	const auto fits = [&](size_t, size_t n, size_t indent) {
		return static_cast<float>(n + indent) * glyph <= l.maxWidth;
	};
	Check(std::strlen(line) < FEED_WRAP && std::strlen(line) * glyph > l.maxWidth,
	      "under FEED_WRAP, and still wider than the room on that window");

	FeedRow rows[FEED_ROWS_MAX];
	size_t  n = FeedRows(line, fits, rows);
	Check(n == 2 && std::string(line + rows[0].from, rows[0].count) ==
	                    "noxx3 is on Give Me Liberty, and you are in" &&
	          std::string(line + rows[1].from, rows[1].count) == "it",
	      "so it goes on two rows, broken at a space, and nothing is lost");

	n = FeedRows("you passed Give Me Liberty", fits, rows);
	Check(n == 1 && rows[0].from == 0 && rows[0].count == 26, "one that fits is one row");

	std::string words;
	while (words.size() < 200)
		words += "word ";
	words.pop_back();
	n = FeedRows(words.c_str(), fits, rows);
	bool every = true;
	for (size_t i = 0; i + 1 < n; ++i)
		every = every && static_cast<float>(rows[i].count + (i ? 2 : 0)) * glyph <= l.maxWidth &&
		        words[rows[i].from + rows[i].count] == ' ';
	Check(n == FEED_ROWS_MAX && every, "a long one takes at most four rows, each broken between words");
	Check(rows[n - 1].from + rows[n - 1].count == words.size(),
	      "and the last row carries the rest, to be cut where it is drawn");

	const std::string oneWord(60, 'x');
	n = FeedRows(oneWord.c_str(), fits, rows);
	Check(n == 2 && rows[0].count + rows[1].count == 60 && rows[0].count * glyph <= l.maxWidth,
	      "a word wider than a row is split, losing nothing");
	Check(FeedRowBreak("abc", 3, [](size_t) { return false; }) == 1,
	      "and a row too narrow for anything still takes a character");
}

} // namespace

int RunChatFeedTests() {
	TestWhatCFontIsHanded();
	TestWhatAKeyboardTypes();
	TestTheLines();
	TestALongMessageWraps();
	TestANoticeIsBrokenWhereTheFontSaysSo();
	TestTheFeedKeepsTheLast();
	TestTheFade();
	TestTheLineBeingTyped();
	TestAPaste();
	TestUpAndDown();
	TestWhereItGoes();
	TestTheColours();
	TestWhereTheScoreboardGoes();
	TestTheScoreboardBands();
	TestTheScoreboardRows();
	TestThePingBars();
	TestTheCommands();
	TestTheVersionMark();
	TestTheChatLineInATallWindow();
	return g_chatFailures;
}
