#include "chat.h"

#include "addresses.h"
#include "pause.h"
#include "population.h"
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
bool     g_listShown = false;
// A finished line, until Client::SendTypedChat takes it.
char     g_outbox[CHAT_LEN] = {};
bool     g_outboxFull = false;

bool g_saidHooked = false;
bool g_saidOpened = false;
bool g_saidPasted = false;

constexpr Rgba CHAT_INK   {233, 230, 222, 255};
constexpr Rgba NOTICE_INK {170, 123, 87, 255};
constexpr Rgba TYPING_INK {255, 255, 255, 255};
constexpr Rgba TITLE_INK  {186, 101, 50, 255};
constexpr Rgba MARK_INK   {160, 160, 160, 255};
constexpr Rgba SHADOW_INK {0, 0, 0, 255};

// Grey and see-through: there to be read off a screenshot, not to be looked at.
constexpr uint8_t MARK_ALPHA = 190;

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
	for (int i = 0; i < n; ++i)
		g_line.Type(out[i]);
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
		if (static_cast<int>(wp) == g_chatKey && MayOpen()) {
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
			g_listShown = !g_listShown;
	}
	return g_wide ? CallWindowProcW(g_prevProc, hwnd, msg, wp, lp)
	              : CallWindowProcA(g_prevProc, hwnd, msg, wp, lp);
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

void DrawFeedLine(const FeedLine &line, float y, uint8_t alpha, const FeedLayout &l) {
	if (line.kind == FeedKind::Notice) {
		PrintLine(l.left, y, line.text, NOTICE_INK, alpha, l);
		return;
	}
	float x = l.left;
	if (line.nickLen != 0 && line.playerId != INVALID_PLAYER) {
		const NickColour c = ChatNickColour(line.playerId);
		x += PrintRun(x, y, line.text, line.nickLen, Rgba{c.r, c.g, c.b, 255}, alpha,
		              l.maxWidth, l.shadow);
	}
	const char *rest = line.text + line.nickLen;
	if (*rest)
		PrintRun(x, y, rest, FEED_MESSAGE, CHAT_INK, alpha, l.maxWidth - (x - l.left), l.shadow);
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

	// Newest at the bottom, just above the line being typed.
	float y = l.bottom - l.lineH;
	for (size_t i = feed.Count(); i-- > 0;) {
		const FeedLine &line  = feed.Line(i);
		const uint8_t   alpha = FeedAlpha(nowMs - line.atMs, typing);
		if (alpha != 0)
			DrawFeedLine(line, y, alpha, l);
		y -= l.lineH;
	}

	if (typing)
		DrawInput(l, nowMs);
}

void DrawPlayerList(const Client &client, float screenH, const FeedLayout &l) {
	float y = screenH * 0.18f;
	char  row[FEED_MESSAGE];

	uint32_t others = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (id != client.LocalPlayerId() && client.PlayerSlot(id).active)
			++others;
	std::snprintf(row, sizeof row, "players - %u here with you", static_cast<unsigned>(others));
	PrintLine(l.left, y, row, TITLE_INK, 255, l);
	y += l.lineH;

	const std::string &me = client.LocalNick();
	char               mine[NICK_LEN + 8];
	std::snprintf(mine, sizeof mine, "%s (you)", me.empty() ? "you" : me.c_str());
	std::snprintf(row, sizeof row, "%s", mine);
	if (client.LocalWanted() > 0) {
		const size_t n = std::strlen(row);
		std::snprintf(row + n, sizeof row - n, "  wanted %u",
		              static_cast<unsigned>(client.LocalWanted()));
	}
	AppendPing(row, sizeof row, client.PingOf(client.LocalPlayerId()));
	PrintLine(l.left, y, row, CHAT_INK, 255, l);
	y += l.lineH;

	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == client.LocalPlayerId())
			continue;
		const RemotePlayer &p = client.PlayerSlot(id);
		if (!p.active)
			continue;
		const uint32_t quiet = p.haveState ? WallClock::NowMs() - p.heardAtMs : 0;
		FormatPlayerRow(row, sizeof row, p.nick.c_str(), p.haveState ? p.last.health : 100.0f,
		                WantedFromFlags(p.last.flags), p.dead,
		                p.seatVehicleNetId != INVALID_NETID, client.PingOf(id), quiet,
		                client.DesyncOf(id));
		const NickColour c = ChatNickColour(id);
		PrintLine(l.left, y, row, Rgba{c.r, c.g, c.b, 255}, 255, l);
		y += l.lineH;
	}

	// What this machine is doing for the session, for the question "why is
	// that car standing still on my screen": how much is going each way, and
	// how much of the city this machine hosts and holds.
	std::snprintf(row, sizeof row, "net  out %llu  in %llu  host %u peds %u cars  hold %u cars",
	              static_cast<unsigned long long>(client.PacketsSent()),
	              static_cast<unsigned long long>(client.PacketsReceived()),
	              static_cast<unsigned>(HostedAmbientPedCount()),
	              static_cast<unsigned>(HostedAmbientCarCount()),
	              static_cast<unsigned>(AmbientCarReplicaCount()));
	PrintLine(l.left, y + l.lineH * 0.5f, row, NOTICE_INK, 255, l);

	// And the copy of anything else furthest from where its owner has it,
	// when the server says one is somewhere else here.
	uint16_t       copyNetId = INVALID_NETID;
	const char    *copyKind  = "";
	const uint16_t copyCm    = client.WorstCopyDesync(copyNetId, copyKind);
	if (copyCm != DESYNC_UNKNOWN && copyCm >= DESYNC_SHOW_CM_ON_LIST) {
		std::snprintf(row, sizeof row, "furthest copy  %s %u", copyKind,
		              static_cast<unsigned>(copyNetId));
		AppendDesync(row, sizeof row, copyCm);
		PrintLine(l.left, y + l.lineH * 1.5f, row, NOTICE_INK, 255, l);
	}
}

void DrawVersionMark(float screenW, float screenH) {
	const MarkLayout m = MeasureVersionMark(screenW, screenH);
	Func<FontScaleFn>(CFont__SetScale)(m.scaleX, m.scaleY);
	PrintRun(m.x, m.y, VERSION_MARK, FEED_MESSAGE, MARK_INK, MARK_ALPHA, screenW, 1.0f);
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

	const float      wrapWas = Global<float>(CFont__Details + FONTDETAILS_WRAPX);
	const FeedLayout l       = MeasureFeed(screenW, screenH);
	FontStateFor(screenW, l.scaleX, l.scaleY);

	DrawFeed(client, l);
	if (g_listShown)
		DrawPlayerList(client, screenH, l);
	if (g_markShown)
		DrawVersionMark(screenW, screenH);

	Func<FontFloatFn>(CFont__SetWrapx)(wrapWas);
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
