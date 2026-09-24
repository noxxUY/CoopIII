#include "scoreboard.h"

#include "addresses.h"
#include "population.h"
#include "../boardlayout.h"
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
// CRect's memory order, addresses.h: left, bottom (y max), right, top (y min).
struct Rect {
	float left, bottom, right, top;
};

using DrawRectFn  = void(__cdecl *)(const Rect *, const Rgba *);
using FontScaleFn = void(__cdecl *)(float, float);
using FontFloatFn = void(__cdecl *)(float);
using FontIntFn   = void(__cdecl *)(int);
using FontVoidFn  = void(__cdecl *)();
using FontColorFn = void(__cdecl *)(const Rgba *);
using FontPrintFn = void(__cdecl *)(float, float, const uint16_t *);
using FontWidthFn = float(__cdecl *)(const uint16_t *, int);
using FindPedFn   = void *(__cdecl *)();

int         g_key    = VK_TAB;
bool        g_pinned = false;
std::string g_server;
bool        g_saidShown = false;

// The HUD's own colours (re3 Hud.cpp, the CRGBA globals at the top), so the
// panel reads as part of GTA III rather than something laid over it.
constexpr Rgba HUD_ORANGE {186, 101, 50, 255};    // HEALTH_COLOR
constexpr Rgba HUD_BLUE   {89, 115, 150, 255};    // MONEY_COLOR
constexpr Rgba HUD_ARMOUR {124, 140, 95, 255};    // ARMOUR_COLOR
constexpr Rgba HUD_WANTED {193, 164, 120, 255};   // WANTED_COLOR
constexpr Rgba HUD_CAR    {194, 165, 120, 255};   // VEHICLE_COLOR

constexpr Rgba PANEL      {8, 10, 14, 205};
constexpr Rgba TITLE_BAR  {0, 0, 0, 150};
constexpr Rgba ROW_STRIPE {255, 255, 255, 10};
constexpr Rgba ROW_YOU    {186, 101, 50, 46};
constexpr Rgba BAR_BACK   {0, 0, 0, 170};
constexpr Rgba OFF        {70, 72, 78, 210};
constexpr Rgba RULE       {186, 101, 50, 110};

constexpr Rgba INK        {233, 230, 222, 255};
constexpr Rgba INK_DIM    {150, 150, 150, 255};
constexpr Rgba INK_AWAY   {115, 115, 115, 255};
constexpr Rgba INK_DEAD   {205, 70, 55, 255};
constexpr Rgba SHADOW     {0, 0, 0, 255};
constexpr Rgba ON_BADGE   {255, 255, 255, 255};

constexpr Rgba PING_GOOD  {110, 200, 90, 255};
constexpr Rgba PING_OK    {235, 195, 70, 255};
constexpr Rgba PING_BAD   {225, 80, 60, 255};

bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Held, in the game's own window, and not the Tab of an Alt+Tab on the way out.
bool KeyHeld() {
	HWND front = GetForegroundWindow();
	if (!front || GetWindowThreadProcessId(front, nullptr) != GetCurrentThreadId())
		return false;
	return Held(g_key) && !Held(VK_MENU);
}

bool ShouldDraw() {
	if (Global<int>(gGameState) != GS_PLAYING_GAME)
		return false;
	if (Global<uint8_t>(CMenuManager__m_bMenuActive) != 0)
		return false;
	if (Global<uint8_t>(CHud__m_Wants_To_Draw_Hud) == 0 ||
	    Global<uint8_t>(TheCamera + CAMERA_WIDESCREEN_ON) != 0)
		return false;
	return Global<void *>(CFont__Sprite + FONT_BANK * SIZEOF_SPRITE2D) != nullptr &&
	       Global<void *>(CFont__Sprite + FONT_HEADING * SIZEOF_SPRITE2D) != nullptr;
}

// ---- engine wrappers ----------------------------------------------------------

void Fill(float x0, float y0, float x1, float y1, Rgba c) {
	if (!(x1 > x0) || !(y1 > y0))
		return;
	const Rect r{x0, y1, x1, y0};
	Func<DrawRectFn>(CSprite2d__DrawRect)(&r, &c);
}

void Font(int16_t style, float sx, float sy, const BoardLayout &b) {
	Func<FontIntFn>(CFont__SetFontStyle)(style);
	Func<FontScaleFn>(CFont__SetScale)(sx * b.unit, sy * b.unit);
}

// Everything CFont has that a printer is expected to set, as chat.cpp and
// nametag.cpp set it. wrapX is put back by the caller.
void FontState(float screenW) {
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
	// The HUD sets and clears its own around the one print that uses it
	// (0x0050885D / 0x0050895D); this runs after the HUD, so it makes sure.
	Func<FontIntFn>(CFont__SetDropShadowPosition)(0);
}

constexpr size_t TEXT_CAP = NICK_LEN + 48;

void Widen(const char *s, uint16_t (&out)[TEXT_CAP], size_t count = TEXT_CAP) {
	size_t n = 0;
	for (; s && s[n] != '\0' && n < count && n + 1 < TEXT_CAP; ++n)
		out[n] = static_cast<uint8_t>(FeedGlyph(static_cast<unsigned char>(s[n]), n == 0));
	out[n] = 0;
}

float Width(const uint16_t *s) { return Func<FontWidthFn>(CFont__GetStringWidth)(s, 1); }

float Width(const char *s) {
	uint16_t w[TEXT_CAP];
	Widen(s, w);
	return Width(w);
}

void Ink(Rgba c, uint8_t alpha) {
	c.a = static_cast<uint8_t>(c.a * alpha / 255);
	Func<FontColorFn>(CFont__SetColor)(&c);
}

// `s` widened for CFont and cut from the end until it is no wider than
// `maxW`. How wide it came out.
float Fit(const char *s, float maxW, uint16_t (&w)[TEXT_CAP]) {
	Widen(s, w);
	size_t n = 0;
	while (w[n])
		++n;
	float width = Width(w);
	while (n > 0 && width > maxW) {
		w[--n] = 0;
		width = Width(w);
	}
	return n == 0 ? 0.0f : width;
}

// Printed once in black a pixel down and right, then in colour: the HUD's own
// drop shadow.
void PrintWide(float x, float y, const uint16_t *w, Rgba ink, uint8_t alpha) {
	if (w[0] == 0)
		return;
	Ink(SHADOW, alpha);
	Func<FontPrintFn>(CFont__PrintString)(x + 1.0f, y + 1.0f, w);
	Ink(ink, alpha);
	Func<FontPrintFn>(CFont__PrintString)(x, y, w);
}

float Print(float x, float y, const char *s, Rgba ink, float maxW = 1e9f, uint8_t alpha = 255) {
	uint16_t w[TEXT_CAP];
	const float width = Fit(s, maxW, w);
	PrintWide(x, y, w, ink, alpha);
	return width;
}

void PrintRight(float right, float y, const char *s, Rgba ink, float maxW = 1e9f) {
	uint16_t w[TEXT_CAP];
	const float width = Fit(s, maxW, w);
	PrintWide(right - width, y, w, ink, 255);
}

// ---- one row ------------------------------------------------------------------

struct Row {
	uint8_t     id      = INVALID_PLAYER;
	const char *nick    = "";
	bool        you     = false;
	bool        host    = false;
	float       health  = 100.0f;
	float       armour  = 0.0f;
	uint8_t     stars   = 0;
	BoardState  state   = BoardState::OnFoot;
	uint32_t    quietMs = 0;
	uint16_t    pingMs  = PING_NONE;
};

Rgba StateInk(BoardState s) {
	switch (s) {
	case BoardState::Driving:
	case BoardState::Passenger: return HUD_CAR;
	case BoardState::Wasted:    return INK_DEAD;
	case BoardState::Away:      return INK_AWAY;
	case BoardState::OnFoot:    break;
	}
	return INK_DIM;
}

Rgba ToneInk(PingTone t) {
	switch (t) {
	case PingTone::Good: return PING_GOOD;
	case PingTone::Ok:   return PING_OK;
	case PingTone::Bad:  return PING_BAD;
	case PingTone::None: break;
	}
	return OFF;
}

// The rectangles of a row. Text is a second pass, after every rectangle.
void DrawRowShapes(const BoardLayout &b, int i, const Row &r) {
	const float top = b.RowTop(i);
	const float bot = top + b.U(BOARD_ROW_H);
	const float mid = (top + bot) * 0.5f;

	if (r.you)
		Fill(b.left, top, b.left + b.width, bot, ROW_YOU);
	else if (i % 2 == 1)
		Fill(b.left, top, b.left + b.width, bot, ROW_STRIPE);

	const NickColour c = ChatNickColour(r.id);
	Fill(b.X(BOARD_CHIP_X), top + b.U(3.0f), b.X(BOARD_CHIP_X + BOARD_CHIP_W), bot - b.U(3.0f),
	     Rgba{c.r, c.g, c.b, 255});

	if (r.host)
		Fill(b.X(BOARD_HOST_X), mid - b.U(BOARD_BADGE_H * 0.5f), b.X(BOARD_HOST_X + BOARD_HOST_W),
		     mid + b.U(BOARD_BADGE_H * 0.5f), HUD_ORANGE);

	// Health over armour, both the HUD's colours, on a dark trough.
	const bool  alive  = r.state != BoardState::Wasted;
	const float barsH  = BOARD_HEALTH_BAR_H + BOARD_BAR_GAP + BOARD_ARMOUR_BAR_H;
	const float hTop   = mid - b.U(barsH * 0.5f);
	const float hBot   = hTop + b.U(BOARD_HEALTH_BAR_H);
	const float x0     = b.X(BOARD_HEALTH_X);
	const float x1     = b.X(BOARD_HEALTH_X + BOARD_HEALTH_W);
	const float edge   = b.unit > 1.5f ? 1.0f : 0.0f;
	Fill(x0 - edge, hTop - edge, x1 + edge, hBot + edge, BAR_BACK);
	if (alive)
		Fill(x0, hTop, x0 + (x1 - x0) * BarFill(r.health), hBot,
		     r.state == BoardState::Away ? OFF : HUD_ORANGE);
	const float aTop = hBot + b.U(BOARD_BAR_GAP);
	const float aBot = aTop + b.U(BOARD_ARMOUR_BAR_H);
	if (alive && r.armour > 0.0f) {
		Fill(x0 - edge, aTop, x1 + edge, aBot + edge, BAR_BACK);
		Fill(x0, aTop, x0 + (x1 - x0) * BarFill(r.armour), aBot, HUD_ARMOUR);
	}

	// Signal bars, bottom-aligned, lit up to the level in the tone of the ping.
	const PingSignal sig  = SignalOf(r.pingMs);
	const Rgba       lit  = ToneInk(sig.tone);
	const float      base = mid + b.U(4.5f);
	for (int k = 0; k < BOARD_PING_BARS; ++k) {
		const float bx = BOARD_PING_X + k * (BOARD_PING_BAR_W + BOARD_PING_BAR_GAP);
		const float bh = BOARD_PING_BAR_H0 + 2.0f * static_cast<float>(k);
		Fill(b.X(bx), base - b.U(bh), b.X(bx + BOARD_PING_BAR_W), base, k < sig.bars ? lit : OFF);
	}
}

void DrawRowText(const BoardLayout &b, int i, const Row &r) {
	const float   top   = b.RowTop(i);
	const uint8_t alpha = r.state == BoardState::Away ? 170 : 255;

	Font(FONT_BANK, BOARD_TEXT_SX, BOARD_TEXT_SY, b);
	const float textY = b.TextTop(top, BOARD_ROW_H, BOARD_TEXT_SY);
	Print(b.X(BOARD_NICK_X), textY, r.nick[0] ? r.nick : "?", INK,
	      b.U(r.host ? BOARD_HOST_X - BOARD_NICK_X - 3.0f : BOARD_HEALTH_X - BOARD_NICK_X - 4.0f),
	      alpha);

	Font(FONT_BANK, BOARD_SMALL_SX, BOARD_SMALL_SY, b);
	const float smallY = b.TextTop(top, BOARD_ROW_H, BOARD_SMALL_SY);
	if (r.host) {
		const float w = Width("HOST");
		const float x = b.X(BOARD_HOST_X) + (b.U(BOARD_HOST_W) - w) * 0.5f;
		Ink(ON_BADGE, 255);
		uint16_t s[TEXT_CAP];
		Widen("HOST", s);
		Func<FontPrintFn>(CFont__PrintString)(x, smallY, s);
	}

	char label[24];
	StateLabel(label, sizeof label, r.state, r.quietMs);
	Print(b.X(BOARD_STATE_X), smallY, label, StateInk(r.state), b.U(BOARD_STATE_W), alpha);

	PingLabel(label, sizeof label, r.pingMs);
	if (label[0])
		Print(b.X(BOARD_PING_TEXT_X), smallY, label, INK_DIM, b.U(BOARD_WIDTH - BOARD_PING_TEXT_X),
		      alpha);

	// The stars, the HUD's own glyph. Unlit ones stay, dim, so six always
	// reads as the scale.
	Font(FONT_HEADING, BOARD_STAR_SX, BOARD_STAR_SY, b);
	const float starY = b.TextTop(top, BOARD_ROW_H, BOARD_STAR_SY);
	const uint8_t lit = StarsLit(r.stars);
	for (uint8_t k = 0; k < 6; ++k)
		Print(b.X(BOARD_STARS_X + k * BOARD_STAR_STEP), starY, "]", k < lit ? HUD_WANTED : OFF,
		      1e9f, alpha);
}

// ---- the roster, read -----------------------------------------------------------

Row LocalRow(const Client &client) {
	Row r;
	r.id     = client.LocalPlayerId();
	r.nick   = client.LocalNick().c_str();
	r.you    = true;
	r.host   = client.IsHost();
	r.stars  = client.LocalWanted();
	r.pingMs = client.PingOf(r.id);

	bool    seated = false;
	uint8_t seat   = 0;
	if (void *ped = Func<FindPedFn>(FindPlayerPed)()) {
		r.health = Field<float>(ped, offs::PED_HEALTH);
		r.armour = Field<float>(ped, offs::PED_ARMOUR);
		void *car = Field<bool>(ped, offs::PED_IN_VEHICLE) ? Field<void *>(ped, offs::PED_MY_VEHICLE)
		                                                   : nullptr;
		seated = car != nullptr;
		seat   = car && Field<void *>(car, offs::VEH_DRIVER) == ped ? 0 : 1;
	}
	r.state = StateOf(false, r.health, seated, seat, 0);
	return r;
}

Row RemoteRow(const Client &client, uint8_t id, uint32_t nowMs) {
	const RemotePlayer &p = client.PlayerSlot(id);
	Row r;
	r.id      = id;
	r.nick    = p.nick.c_str();
	r.host    = client.HostPlayerId() == id;
	r.health  = p.haveState ? p.last.health : 100.0f;
	r.armour  = p.haveState ? p.last.armour : 0.0f;
	r.stars   = p.haveState ? WantedFromFlags(p.last.flags) : 0;
	r.quietMs = p.haveState ? nowMs - p.heardAtMs : 0;
	r.pingMs  = client.PingOf(id);
	r.state   = StateOf(p.dead, r.health, p.seatVehicleNetId != INVALID_NETID, p.seatIndex,
	                    r.quietMs);
	return r;
}

// The copy furthest from where its owner has it - a player, a car, a
// pedestrian - when the server says one is a metre or more out.
void FooterLines(const Client &client, char (&copy)[96]) {
	copy[0] = '\0';
	uint16_t       netId = INVALID_NETID;
	const char    *kind  = "";
	uint16_t       cm    = client.WorstCopyDesync(netId, kind);
	const char    *nick  = nullptr;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == client.LocalPlayerId() || !client.PlayerSlot(id).active)
			continue;
		const uint16_t off = client.DesyncOf(id);
		if (off != DESYNC_UNKNOWN && (cm == DESYNC_UNKNOWN || off > cm)) {
			cm   = off;
			nick = client.PlayerSlot(id).nick.c_str();
		}
	}
	if (cm == DESYNC_UNKNOWN || cm < DESYNC_SHOW_CM_ON_LIST)
		return;
	if (nick)
		std::snprintf(copy, sizeof copy, "furthest copy  player %s", nick[0] ? nick : "?");
	else
		std::snprintf(copy, sizeof copy, "furthest copy  %s %u", kind,
		              static_cast<unsigned>(netId));
	AppendDesync(copy, sizeof copy, cm);
}

void Draw(const Client &client, float screenW, float screenH) {
	const uint32_t nowMs = WallClock::NowMs();

	bool active[MAX_PLAYERS] = {};
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		active[id] = client.PlayerSlot(id).active;
	uint8_t order[MAX_PLAYERS];
	const uint8_t local = client.IsConnected() ? client.LocalPlayerId() : INVALID_PLAYER;
	int           count = BoardOrder(local, active, order);

	Row rows[MAX_PLAYERS];
	int n = 0;
	if (local >= MAX_PLAYERS)
		rows[n++] = LocalRow(client);   // not in a session yet: just you
	for (int i = 0; i < count && n < MAX_PLAYERS; ++i)
		rows[n++] = order[i] == local ? LocalRow(client) : RemoteRow(client, order[i], nowMs);
	count = n;

	char copy[96];
	FooterLines(client, copy);

	const BoardLayout b = MeasureBoard(screenW, screenH, count, copy[0] ? 2 : 1);
	if (!(b.unit > 0.0f))
		return;

	if (!g_saidShown) {
		g_saidShown = true;
		Log("scoreboard: up for the first time, %d row(s), %.0fx%.0f at (%.0f, %.0f) on a "
		    "%.0fx%.0f screen, %.2f px per unit",
		    count, b.width, b.height, b.left, b.top, screenW, screenH, b.unit);
	}

	// Whatever text the HUD has queued is drawn now, so the panel goes over
	// it rather than under it.
	Func<FontVoidFn>(CFont__DrawFonts)();

	// ---- rectangles ----
	const float right = b.left + b.width;
	Fill(b.left, b.top, right, b.top + b.height, PANEL);
	Fill(b.left, b.titleTop, right, b.accentTop, TITLE_BAR);
	Fill(b.left, b.accentTop, right, b.subTop, HUD_ORANGE);
	for (int i = 0; i < count; ++i)
		DrawRowShapes(b, i, rows[i]);
	Fill(b.X(BOARD_PAD), b.footTop, right - b.U(BOARD_PAD), b.footTop + b.U(BOARD_ACCENT_H) * 0.7f,
	     RULE);

	// ---- text ----
	const float wrapWas = Global<float>(CFont__Details + FONTDETAILS_WRAPX);
	FontState(screenW);

	Font(FONT_HEADING, BOARD_TITLE_SX, BOARD_TITLE_SY, b);
	const float titleY = b.TextTop(b.titleTop, BOARD_TITLE_H, BOARD_TITLE_SY);
	// Capitals: this face's lowercase is another style (nametag.h, TagGlyph).
	Print(b.X(BOARD_PAD + 2.0f), titleY, "COOPIII", HUD_ORANGE);
	// FONT_HEADING has no '/' (it came out as "2x8"), so the count is in the
	// bank face, a little larger than the rest of the small text.
	char line[96];
	FormatCount(line, sizeof line, static_cast<unsigned>(count));
	Font(FONT_BANK, BOARD_COUNT_SX, BOARD_COUNT_SY, b);
	PrintRight(right - b.U(BOARD_PAD + 2.0f), b.TextTop(b.titleTop, BOARD_TITLE_H, BOARD_COUNT_SY),
	           line, HUD_BLUE);

	Font(FONT_BANK, BOARD_SMALL_SX, BOARD_SMALL_SY, b);
	const float subY = b.TextTop(b.subTop, BOARD_SUB_H, BOARD_SMALL_SY);
	std::snprintf(line, sizeof line, "%s %s", client.IsConnected() ? "server" : "connecting to",
	              g_server.empty() ? "?" : g_server.c_str());
	Print(b.X(BOARD_PAD + 2.0f), subY, line, INK_DIM, b.U(BOARD_WIDTH * 0.5f - BOARD_PAD));
	const uint8_t hostId = client.HostPlayerId();
	if (client.IsConnected() && hostId < MAX_PLAYERS) {
		const char *hostNick = hostId == local ? client.LocalNick().c_str()
		                                       : client.PlayerSlot(hostId).nick.c_str();
		std::snprintf(line, sizeof line, "world hosted by %s", hostNick[0] ? hostNick : "?");
		PrintRight(right - b.U(BOARD_PAD + 2.0f), subY, line, INK_DIM,
		           b.U(BOARD_WIDTH * 0.5f - BOARD_PAD - 4.0f));
	}

	const float headY = b.TextTop(b.headTop, BOARD_HEAD_H, BOARD_SMALL_SY);
	Print(b.X(BOARD_NICK_X), headY, "PLAYER", HUD_BLUE);
	Print(b.X(BOARD_HEALTH_X), headY, "HEALTH", HUD_BLUE);
	Print(b.X(BOARD_STARS_X), headY, "WANTED", HUD_BLUE);
	Print(b.X(BOARD_STATE_X), headY, "STATUS", HUD_BLUE);
	Print(b.X(BOARD_PING_X), headY, "PING", HUD_BLUE);

	for (int i = 0; i < count; ++i)
		DrawRowText(b, i, rows[i]);

	Font(FONT_BANK, BOARD_SMALL_SX, BOARD_SMALL_SY, b);
	const float footY = b.TextTop(b.FootLineTop(0), BOARD_FOOT_H, BOARD_SMALL_SY);
	Print(b.X(BOARD_PAD + 2.0f), footY, VERSION_MARK, INK_DIM);
	const float room = b.width - b.U(2.0f * (BOARD_PAD + 2.0f) + 8.0f);
	if (copy[0])
		PrintRight(right - b.U(BOARD_PAD + 2.0f),
		           b.TextTop(b.FootLineTop(1), BOARD_FOOT_H, BOARD_SMALL_SY), copy, HUD_WANTED,
		           room);

	Func<FontFloatFn>(CFont__SetWrapx)(wrapWas);
}

} // namespace

void SetScoreboardKey(int vk) {
	if (vk > 0 && vk < 256)
		g_key = vk;
}

void SetScoreboardServer(const std::string &host, uint16_t port) {
	char s[96];
	std::snprintf(s, sizeof s, "%s:%u", host.c_str(), static_cast<unsigned>(port));
	g_server = s;
}

void ToggleScoreboardPin() {
	g_pinned = !g_pinned;
}

void DrawScoreboard(const Client &client) {
	if (!g_pinned && !KeyHeld())
		return;
	if (!ShouldDraw())
		return;
	const float screenW = static_cast<float>(Global<int32_t>(RsGlobal__maximumWidth));
	const float screenH = static_cast<float>(Global<int32_t>(RsGlobal__maximumHeight));
	if (!(screenW > 0.0f) || !(screenH > 0.0f))
		return;
	Draw(client, screenW, screenH);
}

} // namespace coopiii::game
