#include "chat.h"

#include "addresses.h"
#include "cheats.h"
#include "fontcull.h"
#include "pause.h"
#include "scoreboard.h"
#include "../chatfeed.h"
#include "../clock.h"
#include "../log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace coopiii::game {

namespace {

struct Rgba {
	uint8_t r, g, b, a;
};

using FontScaleFn = void(__cdecl *)(float, float);
using FontFloatFn = void(__cdecl *)(float);
using FontIntFn   = void(__cdecl *)(int);
using FontVoidFn  = void(__cdecl *)();
using FontColorFn = void(__cdecl *)(const Rgba *);
using FontPrintFn = void(__cdecl *)(float, float, const uint16_t *);
using FontWidthFn = float(__cdecl *)(const uint16_t *, int);

const Client *g_client = nullptr;
int           g_chatKey = 'T';
int           g_listKey = VK_F9;
bool          g_markShown = true;

HWND    g_window   = nullptr;
WNDPROC g_prevProc = nullptr;
// The window keeps the character set it was made with: an ANSI window
// subclassed through the W functions would be turned into a Unicode one under
// the game's feet.
bool    g_wide     = false;

ChatLine g_line;
// A finished line, until Client::SendTypedChat takes it.
char     g_outbox[CHAT_LEN] = {};
bool     g_outboxFull = false;

bool g_saidHooked = false;
bool g_saidOpened = false;
bool g_saidPasted = false;
bool g_saidCheatKey = false;

constexpr Rgba CHAT_INK   {233, 230, 222, 255};
constexpr Rgba NOTICE_INK {170, 123, 87, 255};
constexpr Rgba TYPING_INK {255, 255, 255, 255};
constexpr Rgba TITLE_INK  {186, 101, 50, 255};
constexpr Rgba MARK_INK   {200, 200, 200, 255};
constexpr Rgba SHADOW_INK {0, 0, 0, 255};

// Grey and see-through: there to be read off a screenshot, not to be looked at.
constexpr uint8_t MARK_ALPHA = 225;

// The caret is on for this long and off for as long again.
constexpr uint32_t CARET_BLINK_MS = 500;

bool InAGame() { return Global<int>(gGameState) == GS_PLAYING_GAME; }
bool MenuIsUp() { return Global<uint8_t>(CMenuManager__m_bMenuActive) != 0; }

bool HudShown() {
	return Global<uint8_t>(CHud__m_Wants_To_Draw_Hud) != 0 &&
	       Global<uint8_t>(TheCamera + CAMERA_WIDESCREEN_ON) == 0;
}

// Only where the line would be drawn: a line typed blind under a cutscene's
// bars is a line nobody meant to send.
bool MayOpen() {
	return g_client != nullptr && g_client->IsConnected() && InAGame() && !MenuIsUp() &&
	       HudShown();
}

bool Held(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// Every key up, in the state the menu reads and in the one the next frame's
// CPad::UpdatePads copies it from.
void ClearKeyStates() {
	std::memset(Ptr<void>(CPad__NewKeyState), 0, pad::SIZEOF_KEYSTATE);
	std::memset(Ptr<void>(CPad__TempKeyState), 0, pad::SIZEOF_KEYSTATE);
}

void CloseLine() {
	g_line.Cancel();
	ClearKeyStates();
}

// Whatever text is on the clipboard, into the line at the caret. Opened
// against the game's own window and let go of before anything else happens,
// so nobody else's copy and paste waits on the game.
void PasteClipboard() {
	if (!OpenClipboard(g_window))
		return;
	if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
		if (const auto *text = static_cast<const wchar_t *>(GlobalLock(data))) {
			const size_t n = g_line.Paste(text);
			GlobalUnlock(data);
			if (n != 0 && !g_saidPasted) {
				g_saidPasted = true;
				Log("chat: pasted %u character(s) into the chat line", static_cast<unsigned>(n));
			}
		}
	}
	CloseClipboard();
}

// A key while the line is open. Everything is swallowed: the game gets none
// of it.
void TypeKey(WPARAM vk, LPARAM lp) {
	switch (vk) {
	case VK_RETURN: {
		char text[CHAT_LEN] = {};
		if (g_line.Submit(text) && !g_outboxFull) {
			std::memcpy(g_outbox, text, sizeof g_outbox);
			g_outboxFull = true;
		}
		CloseLine();
		return;
	}
	case VK_ESCAPE:
		CloseLine();
		return;
	case VK_BACK:   g_line.Backspace(); return;
	case VK_LEFT:   g_line.Left();      return;
	case VK_RIGHT:  g_line.Right();     return;
	case VK_HOME:   g_line.Home();      return;
	case VK_END:    g_line.End();       return;
	case VK_UP:     g_line.Older();     return;
	case VK_DOWN:   g_line.Newer();     return;
	case VK_DELETE:
		if (Held(VK_SHIFT))
			g_line.Clear();
		else
			g_line.Delete();
		return;
	case VK_INSERT:
		if (Held(VK_SHIFT))
			PasteClipboard();
		return;
	default:
		break;
	}

	// Ctrl+V, and not AltGr+V: AltGr is Ctrl and Alt together, and on plenty
	// of keyboards it is how a letter is typed at all.
	if (vk == 'V' && Held(VK_CONTROL) && !Held(VK_MENU)) {
		PasteClipboard();
		return;
	}

	BYTE keys[256];
	if (!GetKeyboardState(keys))
		return;
	WCHAR out[4] = {};
	const int n = ToUnicode(static_cast<UINT>(vk), static_cast<UINT>((lp >> 16) & 0xFF),
	                        keys, out, 4, 0);
	bool typed = false;
	for (int i = 0; i < n; ++i)
		typed |= g_line.Type(out[i]);

	// The chat key was a cheat's letter and this line is the rest of it
	// (cheats.h, CheatFinishedInChatLine): the game gets the keys, and the
	// line was never chat.
	if (typed && FinishCheatFromChatLine(EngineCheatCharFor(g_chatKey), g_line.Text()))
		CloseLine();
}

LRESULT CALLBACK ChatWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	const bool repeat = (lp & (1 << 30)) != 0;
	if (g_line.Open()) {
		switch (msg) {
		case WM_KEYDOWN:
			TypeKey(wp, lp);
			return 0;
		case WM_KEYUP:
		case WM_CHAR:
		case WM_DEADCHAR:
			return 0;
		default:
			break;
		}
	} else if (msg == WM_KEYDOWN && !repeat) {
		// Unless it is the last letter of a cheat: then it is the game's key,
		// as it would have been with no chat at all.
		if (static_cast<int>(wp) == g_chatKey && MayOpen() &&
		    ChatKeyWouldFinishCheat(EngineCheatCharFor(g_chatKey))) {
			if (!g_saidCheatKey) {
				g_saidCheatKey = true;
				Log("chat: the chat key finished a cheat, so it went to the game and the "
				    "chat line stayed shut (said once)");
			}
		} else if (static_cast<int>(wp) == g_chatKey && MayOpen()) {
			g_line.Begin();
			ClearKeyStates();
			if (!g_saidOpened) {
				g_saidOpened = true;
				Log("chat: the chat line is open; Enter sends it, Escape drops it, Up and "
				    "Down go through what was sent");
			}
			return 0;
		}
		if (static_cast<int>(wp) == g_listKey && InAGame())
			ToggleScoreboardPin();
	}
	const LRESULT result = g_wide ? CallWindowProcW(g_prevProc, hwnd, msg, wp, lp)
	                              : CallWindowProcA(g_prevProc, hwnd, msg, wp, lp);
	// The game's key handler has had the key and pushed it into its cheat
	// buffer, or not: one of CoopIII's own cheats is read off that buffer
	// (cheats.h, NoticeTypedKeys).
	if (msg == WM_KEYDOWN)
		NoticeTypedKeys();
	return result;
}

// The game's window, once it is active and belongs to this thread - the one
// the frame pump runs on, which is the one the game made it from.
void SubclassWindow() {
	if (g_prevProc)
		return;
	HWND hwnd = GetActiveWindow();
	if (!hwnd || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId())
		return;
	const bool     wide = IsWindowUnicode(hwnd) != FALSE;
	const LONG_PTR ours = reinterpret_cast<LONG_PTR>(&ChatWndProc);
	const LONG_PTR prev = wide ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, ours)
	                           : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, ours);
	if (!prev)
		return;
	g_wide     = wide;
	g_window   = hwnd;
	g_prevProc = reinterpret_cast<WNDPROC>(prev);
	if (!g_saidHooked) {
		g_saidHooked = true;
		Log("chat: reading the chat key (0x%02X) and the list key (0x%02X) off the game's "
		    "window", g_chatKey, g_listKey);
	}
}

// ---- drawing ---------------------------------------------------------------

void FontColor(Rgba c, uint8_t alpha) {
	c.a = static_cast<uint8_t>(c.a * alpha / 255);
	Func<FontColorFn>(CFont__SetColor)(&c);
}

void FontPrint(float x, float y, const uint16_t *s) {
	Func<FontPrintFn>(CFont__PrintString)(x, y, s);
}

float FontWidth(const uint16_t *s) {
	return Func<FontWidthFn>(CFont__GetStringWidth)(s, 1);
}

// nametag.cpp's FontStateForTags, in the subtitle face.
void FontStateFor(float screenW, float scaleX, float scaleY) {
	Func<FontVoidFn>(CFont__SetBackgroundOff)();
	Func<FontVoidFn>(CFont__SetBackGroundOnlyTextOff)();
	Func<FontVoidFn>(CFont__SetJustifyOff)();
	Func<FontVoidFn>(CFont__SetCentreOff)();
	Func<FontVoidFn>(CFont__SetRightJustifyOff)();
	Func<FontFloatFn>(CFont__SetRightJustifyWrap)(0.0f);
	Func<FontFloatFn>(CFont__SetCentreSize)(screenW);
	Func<FontFloatFn>(CFont__SetWrapx)(screenW * 4.0f);
	Func<FontFloatFn>(CFont__SetAlphaFade)(FONT_ALPHA_OPAQUE);
	Func<FontVoidFn>(CFont__SetPropOn)();
	Func<FontIntFn>(CFont__SetFontStyle)(FONT_BANK);
	Func<FontScaleFn>(CFont__SetScale)(scaleX, scaleY);
}

// `count` characters of `text` widened for CFont, each through FeedGlyph on
// the way, whatever it has been through already: CFont's token parser walks
// to the next '~' with no end test, so one reaching it here is a read off the
// end of this buffer.
size_t Widen(const char *text, size_t count, uint16_t *out, size_t cap) {
	size_t n = 0;
	for (; n < count && text[n] != '\0' && n + 1 < cap; ++n)
		out[n] = static_cast<uint8_t>(FeedGlyph(static_cast<unsigned char>(text[n]), n == 0));
	out[n] = 0;
	return n;
}

// Widened and cut to `maxWidth`, so a line that is somehow still too wide is
// shortened rather than wrapped over the one under it.
void WidenToFit(const char *text, size_t count, uint16_t *out, size_t cap, float maxWidth) {
	size_t n = Widen(text, count, out, cap);
	while (n > 0 && FontWidth(out) > maxWidth) {
		const size_t cut = n > 8 ? 8 : 1;
		n -= cut;
		out[n] = 0;
	}
}

// Prints and says how wide it was, for whatever goes on after it.
float PrintRun(float x, float y, const char *text, size_t count, Rgba ink, uint8_t alpha,
               float maxWidth, float shadow) {
	uint16_t wide[FEED_MESSAGE + 2];
	WidenToFit(text, count, wide, FEED_MESSAGE + 2, maxWidth);
	if (wide[0] == 0)
		return 0.0f;
	FontColor(SHADOW_INK, alpha);
	FontPrint(x + shadow, y + shadow, wide);
	FontColor(ink, alpha);
	FontPrint(x, y, wide);
	return FontWidth(wide);
}

void PrintLine(float x, float y, const char *text, Rgba ink, uint8_t alpha, const FeedLayout &l) {
	PrintRun(x, y, text, FEED_MESSAGE, ink, alpha, l.maxWidth, l.shadow);
}

bool ShouldDraw() {
	if (!HudShown() || MenuIsUp())
		return false;
	return Global<void *>(CFont__Sprite + FONT_BANK * SIZEOF_SPRITE2D) != nullptr;
}

// `count` characters of `text`, after `indent` of FEED_CONTINUED, into `out`.
size_t RowText(const char *text, size_t count, size_t indent, char (&out)[FEED_TEXT + 4]) {
	size_t n = 0;
	for (; n < indent && n + 1 < sizeof out; ++n)
		out[n] = FEED_CONTINUED[n];
	for (size_t i = 0; i < count && text[i] != '\0' && n + 1 < sizeof out; ++i)
		out[n++] = text[i];
	out[n] = '\0';
	return n;
}

bool RowFits(const char *text, size_t count, size_t indent, float maxWidth) {
	char     row[FEED_TEXT + 4];
	uint16_t wide[FEED_TEXT + 4];
	const size_t n = RowText(text, count, indent, row);
	Widen(row, n, wide, FEED_TEXT + 4);
	return FontWidth(wide) <= maxWidth;
}

// One row of a line: the first carries the name, in its player's colour.
void DrawFeedRow(const FeedLine &line, const FeedRow &row, bool continued, float y, uint8_t alpha,
                 const FeedLayout &l) {
	char text[FEED_TEXT + 4];
	RowText(line.text + row.from, row.count, continued ? std::strlen(FEED_CONTINUED) : 0, text);
	const bool named = !continued && line.nickLen != 0 && line.playerId != INVALID_PLAYER;
	if (line.kind == FeedKind::Notice && !named) {
		PrintLine(l.left, y, text, NOTICE_INK, alpha, l);
		return;
	}
	float  x    = l.left;
	size_t nick = 0;
	if (named) {
		nick               = line.nickLen < row.count ? line.nickLen : row.count;
		const NickColour c = ChatNickColour(line.playerId);
		x += PrintRun(x, y, text, nick, Rgba{c.r, c.g, c.b, 255}, alpha, l.maxWidth, l.shadow);
	}
	const char *rest = text + nick;
	if (*rest)
		PrintRun(x, y, rest, FEED_MESSAGE, line.kind == FeedKind::Notice ? NOTICE_INK : CHAT_INK,
		         alpha, l.maxWidth - (x - l.left), l.shadow);
}

// The line being typed, from as far along as it takes for the caret to stay
// on the screen, with the caret drawn over the text rather than in it so the
// words do not shuffle along every time it blinks.
void DrawInput(const FeedLayout &l, uint32_t nowMs) {
	constexpr const char *PROMPT = "say: ";
	uint16_t wide[FEED_MESSAGE + 2];

	Widen(PROMPT, std::strlen(PROMPT), wide, FEED_MESSAGE + 2);
	const float promptW = FontWidth(wide);
	PrintLine(l.left, l.bottom, PROMPT, TITLE_INK, 255, l);

	const char  *text  = g_line.Text();
	const size_t caret = g_line.Caret();
	const float  room  = l.maxWidth - promptW;

	size_t from = 0;
	while (from < caret) {
		Widen(text + from, caret - from, wide, FEED_MESSAGE + 2);
		if (FontWidth(wide) <= room)
			break;
		from += from + 4 < caret ? 4 : 1;
	}

	const float x = l.left + promptW;
	PrintRun(x, l.bottom, text + from, FEED_MESSAGE, TYPING_INK, 255, room, l.shadow);

	if ((nowMs / CARET_BLINK_MS) % 2 == 0) {
		Widen(text + from, caret - from, wide, FEED_MESSAGE + 2);
		const float at = caret > from ? FontWidth(wide) : 0.0f;
		PrintRun(x + at, l.bottom, "_", 1, TYPING_INK, 255, l.maxWidth, l.shadow);
	}
}

void DrawFeed(const Client &client, const FeedLayout &l) {
	const ChatFeed &feed   = client.Feed();
	const bool      typing = g_line.Open();
	const uint32_t  nowMs  = WallClock::NowMs();

	// Newest at the bottom, just above the line being typed, each line on as
	// many rows as the font says it takes (chatfeed.h, FeedRows).
	float y = l.bottom - l.lineH;
	for (size_t i = feed.Count(); i-- > 0 && y > 0.0f;) {
		const FeedLine &line  = feed.Line(i);
		const uint8_t   alpha = FeedAlpha(nowMs - line.atMs, typing);
		FeedRow         rows[FEED_ROWS_MAX];
		size_t          count = 1;
		if (alpha != 0)
			count = FeedRows(line.text,
			                 [&](size_t from, size_t n, size_t indent) {
				                 return RowFits(line.text + from, n, indent, l.maxWidth);
			                 },
			                 rows);
		for (size_t r = count; r-- > 0;) {
			if (alpha != 0)
				DrawFeedRow(line, rows[r], r != 0, y, alpha, l);
			y -= l.lineH;
		}
	}

	if (typing)
		DrawInput(l, nowMs);
}

void DrawVersionMark(float screenW, float screenH) {
	const float      cullY = CurrentTextCullLine(screenW, screenH);
	const MarkLayout m     = MeasureVersionMark(screenH, cullY);
	Func<FontScaleFn>(CFont__SetScale)(m.scaleX, m.scaleY);
	const float w = PrintRun(m.x, m.y, VERSION_MARK, FEED_MESSAGE, MARK_INK, MARK_ALPHA, screenW,
	                         1.0f);

	// Once per screen size, so a log from any window says where it went, and
	// where CFont stops printing on it (game/fontcull.h).
	static float saidW = 0.0f, saidH = 0.0f;
	if (screenW != saidW || screenH != saidH) {
		saidW = screenW;
		saidH = screenH;
		Log("chat: version mark at (%.1f, %.1f) scale %.3f x %.3f on a %.0fx%.0f screen, "
		    "%.1f px wide, %s, %s y = %.0f",
		    m.x, m.y, m.scaleX, m.scaleY, screenW, screenH, w,
		    w > 0.0f ? "text not empty" : "text came out EMPTY",
		    m.y < cullY ? "above the cull line at" : "PAST the cull line at", cullY);
	}
}

} // namespace

void SetChatKeys(int chatKey, int listKey) {
	if (chatKey > 0 && chatKey < 256)
		g_chatKey = chatKey;
	if (listKey > 0 && listKey < 256)
		g_listKey = listKey;
}

void SetVersionMarkShown(bool shown) {
	g_markShown = shown;
}

void InstallChat(const Client &client) {
	g_client = &client;
}

void RemoveChat() {
	// Only if ours is still the window's procedure. Somebody who subclassed it
	// after us has our procedure as their "previous" and restoring over them
	// would cut them out.
	if (g_window && g_prevProc) {
		const LONG_PTR ours = reinterpret_cast<LONG_PTR>(&ChatWndProc);
		const LONG_PTR prev = reinterpret_cast<LONG_PTR>(g_prevProc);
		if (g_wide && GetWindowLongPtrW(g_window, GWLP_WNDPROC) == ours)
			SetWindowLongPtrW(g_window, GWLP_WNDPROC, prev);
		else if (!g_wide && GetWindowLongPtrA(g_window, GWLP_WNDPROC) == ours)
			SetWindowLongPtrA(g_window, GWLP_WNDPROC, prev);
	}
	g_line.Cancel();
	HoldControlsForChat(false);
	g_client = nullptr;
}

void TickChat() {
	if (!g_client)
		return;
	SubclassWindow();

	// A line open when the menu comes up or the game stops is dropped rather
	// than left holding the controls.
	if (g_line.Open() && !MayOpen())
		CloseLine();
	if (g_line.Open())
		ClearKeyStates();
	HoldControlsForChat(g_line.Open());
}

void DrawChatOverlay(const Client &client) {
	if (!ShouldDraw())
		return;
	const float screenW = static_cast<float>(Global<int32_t>(RsGlobal__maximumWidth));
	const float screenH = static_cast<float>(Global<int32_t>(RsGlobal__maximumHeight));
	if (!(screenW > 0.0f) || !(screenH > 0.0f))
		return;

	const float wrapWas = Global<float>(CFont__Details + FONTDETAILS_WRAPX);
	FeedLayout  l       = MeasureFeed(screenW, screenH);
	// Only moves when PrintChar's y test could not be fixed and the window is
	// tall enough for the line to be past it.
	l.bottom -= LiftAboveCull(l.bottom + l.shadow, CurrentTextCullLine(screenW, screenH), screenH);
	FontStateFor(screenW, l.scaleX, l.scaleY);

	DrawFeed(client, l);
	if (g_markShown)
		DrawVersionMark(screenW, screenH);

	Func<FontFloatFn>(CFont__SetWrapx)(wrapWas);
}

void DrawCornerMark(const char *text, int row) {
	if (!text || text[0] == '\0' || MenuIsUp())
		return;
	if (Global<void *>(CFont__Sprite + FONT_BANK * SIZEOF_SPRITE2D) == nullptr)
		return;
	const float screenW = static_cast<float>(Global<int32_t>(RsGlobal__maximumWidth));
	const float screenH = static_cast<float>(Global<int32_t>(RsGlobal__maximumHeight));
	if (!(screenW > 0.0f) || !(screenH > 0.0f))
		return;

	const float      wrapWas = Global<float>(CFont__Details + FONTDETAILS_WRAPX);
	const float      cullY   = CurrentTextCullLine(screenW, screenH);
	const MarkLayout scale   = MeasureVersionMark(screenH, cullY);
	FontStateFor(screenW, scale.scaleX, scale.scaleY);
	uint16_t wide[FEED_MESSAGE + 2];
	Widen(text, FEED_MESSAGE, wide, FEED_MESSAGE + 2);
	MarkLayout m = MeasureCornerMark(screenW, screenH, cullY, FontWidth(wide));
	if (row > 0) {
		m.y -= static_cast<float>(row) * FEED_CELL_HEIGHT * m.scaleY * FEED_LINE_GAP;
		if (m.y < 1.0f)
			m.y = 1.0f;
	}
	const float w = PrintRun(m.x, m.y, text, FEED_MESSAGE, MARK_INK, MARK_ALPHA, screenW, 1.0f);
	Func<FontFloatFn>(CFont__SetWrapx)(wrapWas);

	static float saidW = 0.0f, saidH = 0.0f;
	if (screenW != saidW || screenH != saidH) {
		saidW = screenW;
		saidH = screenH;
		Log("chat: corner mark at (%.1f, %.1f) on a %.0fx%.0f screen, %.1f px wide, %s y = %.0f",
		    m.x, m.y, screenW, screenH, w,
		    m.y < cullY ? "above the cull line at" : "PAST the cull line at", cullY);
	}
}

void AddChatToBridge(WorldBridge &bridge) {
	bridge.TakeTypedChat = [](char (&out)[CHAT_LEN]) -> bool {
		if (!g_outboxFull)
			return false;
		std::memcpy(out, g_outbox, sizeof g_outbox);
		g_outboxFull = false;
		return true;
	};
}

} // namespace coopiii::game
