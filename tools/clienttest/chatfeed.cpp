// The chat feed, the line being typed and the version mark: client/src/chatfeed.h.
// The drawing and the keys are game/chat.cpp and need the game; what they draw
// and what they send is all here.

#include "chatfeed.h"

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

void TestThePlayerList() {
	std::printf("\nthe player list\n");
	char row[FEED_MESSAGE];
	FormatPlayerRow(row, sizeof row, "alice", 87.4f, 0, false, false);
	Check(std::strcmp(row, "alice  87 hp") == 0, "a name and what is left of her");
	FormatPlayerRow(row, sizeof row, "alice", 0.3f, 2, false, true);
	Check(std::strcmp(row, "alice  1 hp  in a car  wanted 2") == 0,
	      "alive is never 0 hp, and the car and the stars follow");
	FormatPlayerRow(row, sizeof row, "bob", 50.0f, 3, true, false);
	Check(std::strcmp(row, "bob  dead") == 0, "the dead are dead whatever else was said");
	FormatPlayerRow(row, sizeof row, "", 100.0f, 9, false, false);
	Check(std::strcmp(row, "?  100 hp  wanted 6") == 0,
	      "no nick is a question mark, and six stars is the most");
	FormatPlayerRow(row, sizeof row, "tank", 5000.0f, 0, false, false);
	Check(std::strcmp(row, "tank  999 hp") == 0, "health a script set sky-high stays readable");
	FormatPlayerRow(row, sizeof row, "~x", 10.0f, 0, false, false);
	Check(row[0] == '-', "and a nick cannot carry a token into the list either");
	FormatPlayerRow(row, sizeof row, "carol", 64.0f, 0, false, false, 54);
	Check(std::strcmp(row, "carol  64 hp  54 ms") == 0, "and her round trip on the end");
	FormatPlayerRow(row, sizeof row, "dave", 64.0f, 0, true, false, 1400);
	Check(std::strcmp(row, "dave  dead  1s+") == 0, "a slow one past a second, dead or alive");
	FormatPlayerRow(row, sizeof row, "erin", 64.0f, 0, false, false, PING_NONE);
	Check(std::strcmp(row, "erin  64 hp") == 0, "and none while the server has not said");
	FormatPlayerRow(row, sizeof row, "finn", 64.0f, 0, false, false, 40, QUIET_AFTER_MS - 1);
	Check(std::strcmp(row, "finn  64 hp  40 ms") == 0, "a couple of missed packets say nothing");
	FormatPlayerRow(row, sizeof row, "finn", 64.0f, 0, false, false, 40, 12500);
	Check(std::strcmp(row, "finn  64 hp  quiet 12s  40 ms") == 0,
	      "but a player nothing has come from for a while is marked, and for how long");
	FormatPlayerRow(row, sizeof row, "finn", 64.0f, 0, false, false, PING_NONE, 5000000);
	Check(std::strcmp(row, "finn  64 hp  quiet 999s+") == 0, "without running off the row");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, 40, 0, 740);
	Check(std::strcmp(row, "gus  64 hp  off 7.4 m  40 ms") == 0,
	      "a copy of them somewhere else says how far");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, 40, 0, 99);
	Check(std::strcmp(row, "gus  64 hp  40 ms") == 0, "under a metre is not worth a word");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, PING_NONE, 0, 4210);
	Check(std::strcmp(row, "gus  64 hp  off 42 m") == 0, "past ten metres, whole metres");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, PING_NONE, 0, DESYNC_MAX_CM);
	Check(std::strcmp(row, "gus  64 hp  off 655 m+") == 0, "and the cap says it is the cap");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, PING_NONE, 0, DESYNC_UNKNOWN);
	Check(std::strcmp(row, "gus  64 hp") == 0, "nothing when nothing was compared");
	FormatPlayerRow(row, sizeof row, "gus", 64.0f, 0, false, false, PING_NONE, 12500, 740);
	Check(std::strcmp(row, "gus  64 hp  quiet 12s") == 0,
	      "and nothing for somebody quiet, which already says why");

	const std::string longest(NICK_LEN - 1, 'n');
	const FeedLayout wide = MeasureFeed(1280.0f, 720.0f);
	FormatPlayerRow(row, sizeof row, longest.c_str(), 100.0f, 6, false, true, 999, 999000,
	                DESYNC_MAX_CM);
	Check(std::strlen(row) * 32.0f * wide.scaleX * 0.6f < wide.maxWidth,
	      "the longest row there can be fits across a 16:9 screen");
	FormatPlayerRow(row, sizeof row, longest.c_str(), 100.0f, 6, false, true, 999, 0,
	                DESYNC_MAX_CM);
	Check(std::strlen(row) * 32.0f * wide.scaleX * 0.6f < wide.maxWidth,
	      "and so does the longest with a distance in it");
}

void TestTheVersionMark() {
	std::printf("\nthe version mark\n");
	Check(std::string(VERSION_MARK) == std::string("CoopIII ") + COOPIII_VERSION,
	      "it says CoopIII and the version");
	Check(Copied(VERSION_MARK) == VERSION_MARK, "and CFont can draw every character of it");
	for (const float h : {448.0f, 480.0f, 720.0f, 1080.0f, 2160.0f}) {
		const MarkLayout m = MeasureVersionMark(h * 16.0f / 9.0f, h);
		const float unit   = h / 448.0f;
		const float bottom = m.y + FEED_CELL_HEIGHT * m.scaleY;
		if (!(m.x > 0.0f && bottom < h && m.y > h - RADAR_BOTTOM_UNITS * unit)) {
			Check(false, "in the strip under the radar, at every height");
			return;
		}
	}
	Check(true, "in the strip under the radar, at every height");
	const MarkLayout a = MeasureVersionMark(1280.0f, 720.0f);
	const MarkLayout b = MeasureVersionMark(1920.0f, 720.0f);
	Check(a.scaleY == b.scaleY && a.x == b.x && a.y == b.y, "and a wider screen does not move it");
}

} // namespace

int RunChatFeedTests() {
	TestWhatCFontIsHanded();
	TestWhatAKeyboardTypes();
	TestTheLines();
	TestALongMessageWraps();
	TestTheFeedKeepsTheLast();
	TestTheFade();
	TestTheLineBeingTyped();
	TestAPaste();
	TestUpAndDown();
	TestWhereItGoes();
	TestTheColours();
	TestThePlayerList();
	TestTheVersionMark();
	return g_chatFailures;
}
