// CoopIII server, the window.
//
// design/screens/Server.dc.html is the screen and ServerOptions.dc.html is the
// dialog. Every position and size below is out of those files.
//
// The server itself is server-core, the same class --nogui runs, in this same
// process. This file gives it a log sink, draws what it is doing, and lets
// somebody kick a player or change the rules.
#include "run.h"

#include "config.h"
#include "reach.h"
#include "server.h"

#include "ui/anim.h"
#include "ui/app.h"
#include "ui/citymap.h"
#include "ui/widgets.h"

#include <imgui_internal.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace ui;

namespace coopiii {
namespace {

// The design's window, to the pixel.
constexpr float kTitleBar  = 40.0f;
constexpr float kPadX      = 28.0f;
constexpr float kBandTop   = 24.0f;
constexpr float kBandBot   = 22.0f;
constexpr float kBodyTop   = 4.0f;
constexpr float kBodyBot   = 24.0f;
constexpr float kPlayersW  = 420.0f;
constexpr float kGap       = 20.0f;
constexpr float kRowH      = 63.0f;
constexpr float kHeaderH   = 52.0f;
constexpr float kRulesH    = 72.0f;

// How many lines the console keeps. Past this the oldest go: a dedicated
// server left running overnight should not grow without limit, and the log a
// person actually reads is the last screenful.
constexpr size_t kMaxLines = 2000;

struct Line {
	LogKind     kind;
	std::string time;
	std::string text;
};

// The last thing a slot said, kept for as long as it takes its row to fade
// out. The session forgets a player the instant they go, and a row that
// vanishes between two frames reads as a glitch rather than as a departure.
struct SlotView {
	std::string nick;
	std::string activity;
	uint32_t    ping  = 0;
	bool        shown = false;
};

struct State {
	Server                server;
	ServerConfig          config;
	ServerConfig          editing;     // what the dialog is changing
	std::deque<Line>      lines;
	bool                  running    = false;
	bool                  optionsOpen = false;
	int                   filter     = 0;   // 0 all, 1 players, 2 chat
	size_t                lastCount  = 0;
	std::string           address;
	std::string           startError;
	bool                  copied     = false;
	float                 copiedAt   = 0.0f;
	float                 savedAt    = -10.0f;   // when the rules last changed
	SlotView              slots[MAX_PLAYERS];
};

State g_state;

// ---- the address to share -------------------------------------------------

// The address a player on the same network types in, and the one the design's
// header shows: a private one behind a gateway if there is one
// (PreferredLanAddress), or failing that the first there is.
std::string LanAddress() {
	const std::vector<LocalAddress> all = LocalIPv4Addresses();
	if (const uint32_t lan = PreferredLanAddress(all))
		return FormatAddress(lan);
	for (const LocalAddress &a : all)
		if (KindOf(a.addr) != AddressKind::LinkLocal)
			return FormatAddress(a.addr);
	return "127.0.0.1";
}

std::string Clock(uint32_t ms) {
	const uint32_t seconds = ms / 1000;
	char           out[32];
	std::snprintf(out, sizeof(out), "%u:%02u:%02u", seconds / 3600, (seconds / 60) % 60,
	              seconds % 60);
	return out;
}

std::string Now() {
	const std::time_t t = std::time(nullptr);
	std::tm           local{};
	localtime_s(&local, &t);
	char out[16];
	std::snprintf(out, sizeof(out), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
	return out;
}

bool PassesFilter(LogKind kind, int filter) {
	if (filter == 0)
		return true;
	if (filter == 1)
		return kind == LogKind::Join || kind == LogKind::Leave;
	return kind == LogKind::Chat;
}

void Say(LogKind kind, const char *text) {
	g_state.lines.push_back({kind, Now(), text});
	while (g_state.lines.size() > kMaxLines)
		g_state.lines.pop_front();
}

// ---- starting and stopping ------------------------------------------------

void StartServer() {
	g_state.startError.clear();
	if (!g_state.server.Start(g_state.config.port, g_state.config.friendlyFire,
	                          g_state.config.ammoSync,
	                          WireValue(g_state.config.wantedLevel),
	                          WireValue(g_state.config.rampage),
	                          WireValue(g_state.config.cheats),
	                          WireValue(g_state.config.money),
	                          WireValue(g_state.config.hiddenPackages))) {
		g_state.startError = "Could not listen on that port. Something else may be using it.";
		return;
	}
	g_state.running = true;
	g_state.server.SetPassword(g_state.config.password);
	g_state.address = LanAddress();
	for (const std::string &line : ReachLines(LocalIPv4Addresses(), g_state.config.port))
		Say(LogKind::Info, line.c_str());
}

void StopServer() {
	if (!g_state.running)
		return;
	g_state.server.Stop();
	g_state.running = false;
	Say(LogKind::Info, "stopped");
}

} // namespace

int RunWindow(const Startup &startup) {
	AppOptions options;
	options.title          = "CoopIII Server";
	options.width          = 1200;
	options.height         = 760;
	options.resizable      = true;
	options.maximizeBox    = true;
	options.titleBarHeight = kTitleBar;

	App app(options);
	if (!app.Ok()) {
		MessageBoxW(nullptr,
		            L"CoopIII Server could not create its window. A Direct3D 11 capable "
		            L"display driver is needed.\r\n\r\nRun it with --nogui to start the "
		            L"server without one.",
		            L"CoopIII Server", MB_ICONERROR | MB_OK);
		return 1;
	}

	// The command line has already had its say over the ini - server/main.cpp.
	g_state.config = startup.config;
	g_state.server.SetLogSink([](LogKind kind, const char *line) { Say(kind, line); });

	Say(LogKind::Info, startup.hadConfig ? ("settings from " + startup.configPath).c_str()
	                                     : ("no " + startup.configPath + ", using the defaults")
	                                           .c_str());
	if (startup.portFromArgs)
		Say(LogKind::Detail, "port came from the command line, not the file");
	StartServer();

	// Servicing the session is not part of drawing. A dedicated server spends
	// its life minimised, or behind something, or being dragged across a
	// screen - and App::Run skips the frame for all three. This runs every
	// turn of the loop regardless. 0 so it never blocks.
	const auto tick = [] {
		if (g_state.running)
			g_state.server.Tick(0);
	};

	const int result = app.Run([&] {
		const Theme &theme  = app.CurrentTheme();
		const ImVec2 screen = ImGui::GetIO().DisplaySize;

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(screen);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme.bgWindow));
		ImGui::Begin("##root", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		                 ImGuiWindowFlags_NoBringToFrontOnFocus);

		ImDrawList *draw = ImGui::GetWindowDrawList();
		Session    &session = g_state.server.SessionRef();

		// ---- the header band ----------------------------------------------
		const float bandH = 126.0f;
		const float bandY = kTitleBar;
		draw->AddRectFilled(ImVec2(0, bandY), ImVec2(screen.x, bandY + bandH), theme.bgWindow);

		// The map runs behind the band, faded from the left and from the
		// bottom so neither the title nor the address sits on texture. The
		// design draws it in a 1200 x 170 box and lets the band clip it, and
		// the fades are fractions of that 170 - so the box is 170 here too.
		const ImVec2 mapSize(screen.x, 170.0f);
		draw->PushClipRect(ImVec2(0, bandY), ImVec2(screen.x, bandY + bandH), true);
		CityMap::Draw(draw, ImVec2(0, bandY), mapSize, theme,
		              MapRoute{491.5f, 194.5f, 247.5f, 588.5f}, app.Seconds(), app.Animates());
		CityMap::DrawFade(draw, ImVec2(0, bandY), mapSize, theme,
		                  MapFade{false, 0.20f, 0.44f, theme.bgWindow});
		CityMap::DrawFade(draw, ImVec2(0, bandY), mapSize, theme,
		                  MapFade{true, 0.45f, 0.78f, theme.bgWindow});
		draw->PopClipRect();

		const TitleBarResult bar = TitleBar(app, "Server", "v0.0.1", true);
		if (bar.minimise) app.Minimize();
		if (bar.maximise) app.ToggleMaximize();
		if (bar.close)    app.Close();
		if (bar.theme)    app.SetTheme(!app.IsLight());

		// While the options are up, the screen behind them is a picture. The
		// scrim says as much and this makes it true. The title bar is outside
		// it on purpose: a dialog should never be able to trap the window.
		ImGui::BeginDisabled(g_state.optionsOpen);

		const float titleY = bandY + kBandTop;
		const float titleW = Text(ImVec2(kPadX, titleY), Type::WindowHeading, theme.textPrimary,
		                          "Server");
		if (g_state.running)
			Pill(ImVec2(kPadX + titleW + 14.0f, titleY + 4.0f), "Running", theme.statusOk,
			     theme.okPillBg, theme.okPillBorder, true);
		else
			Pill(ImVec2(kPadX + titleW + 14.0f, titleY + 4.0f), "Stopped", theme.textTertiary,
			     theme.bgSubtle, theme.borderControl);

		char summary[256];
		if (g_state.running)
			std::snprintf(summary, sizeof(summary),
			              "Listening on UDP port %u \xC2\xB7 %u slots \xC2\xB7 protocol v%u "
			              "\xC2\xB7 up %s \xC2\xB7 in-game %02u:%02u",
			              g_state.server.Port(), MAX_PLAYERS, PROTOCOL_VERSION,
			              Clock(g_state.server.UptimeMs()).c_str(), session.Clock().Hour(),
			              session.Clock().Minute());
		else
			std::snprintf(summary, sizeof(summary), "%s",
			              g_state.startError.empty()
			                  ? "Not listening. Press Start server to open the port again."
			                  : g_state.startError.c_str());
		Text(ImVec2(kPadX, titleY + 34.0f + 6.0f), Type::BodySmall,
		     g_state.startError.empty() ? theme.textSecondary : theme.statusFail, summary);

		// Right of the band: the address to share, then the stop button.
		const float rightEdge = screen.x - kPadX;
		const float stopW     = 132.0f;
		const Rect  stopBox{ImVec2(rightEdge - stopW, titleY + 6.0f), ImVec2(stopW, 44.0f)};
		if (g_state.running) {
			if (OutlineButton("##stop", stopBox, "Stop server", Icon::Stop, theme, false,
			                  Type::ButtonSmall, theme.brandRed))
				StopServer();
		} else {
			if (OutlineButton("##start", stopBox, "Start server", Icon::Play, theme, false,
			                  Type::ButtonSmall, theme.statusOk))
				StartServer();
		}

		char address[64];
		std::snprintf(address, sizeof(address), "%s:%u",
		              g_state.address.empty() ? "127.0.0.1" : g_state.address.c_str(),
		              g_state.server.Port());
		const float addrTextW = MeasureText(Type::MonoLarge, address).x;
		const float addrW     = 14.0f + ImMax(addrTextW, 112.0f) + 16.0f + 36.0f + 8.0f;
		const Rect  addrBox{ImVec2(stopBox.pos.x - 12.0f - addrW, titleY), ImVec2(addrW, 56.0f)};
		Card(addrBox, theme, radius::kPrimary);
		Text(Add(addrBox.pos, ImVec2(14.0f, 8.0f)), Type::MetaLabel, theme.textTertiary,
		     "Address to share");
		Text(Add(addrBox.pos, ImVec2(14.0f, 8.0f + 16.0f + 2.0f)), Type::MonoLarge,
		     theme.textPrimary, address);
		// The tick that says it went on the clipboard grows in and the copy
		// icon fades under it, then they swap back a second and a half later.
		const Rect  copyBox{ImVec2(addrBox.Max().x - 8.0f - 36.0f, addrBox.pos.y + 10.0f),
		                    ImVec2(36.0f, 36.0f)};
		const float done = Appear("##copied", g_state.copied, 18.0f);
		if (IconButton("##copy", copyBox, Icon::Copy, theme,
		               WithAlpha(theme.textButton, 1.0f - done), 16.0f, true, theme.bgSubtle)) {
			App::SetClipboard(address);
			g_state.copied   = true;
			g_state.copiedAt = app.Seconds();
		}
		if (done > 0.0f) {
			const DrawGroup tick(draw);
			DrawIcon(draw, Icon::Check, ImVec2(copyBox.pos.x + 10.0f, copyBox.pos.y + 10.0f),
			         16.0f, theme.statusOk);
			tick.Fade(done);
			tick.Scale(copyBox.Centre(), 0.5f + 0.5f * EaseBack(done));
		}
		if (g_state.copied && app.Seconds() - g_state.copiedAt > 1.6f)
			g_state.copied = false;

		char forward[128];
		std::snprintf(forward, sizeof(forward),
		              "For friends outside your network, forward UDP port %u on your router.",
		              g_state.server.Port());
		TextRight(rightEdge, titleY + 56.0f + 8.0f, Type::Hint, theme.textTertiary, forward);

		// ---- the body ------------------------------------------------------
		const float bodyY = bandY + bandH + kBodyTop;
		const float bodyH = screen.y - bodyY - kBodyBot;
		const float rightX = kPadX + kPlayersW + kGap;
		const float rightW = screen.x - kPadX - rightX;

		// Players.
		const Rect playersBox{ImVec2(kPadX, bodyY), ImVec2(kPlayersW, bodyH)};
		Card(playersBox, theme);
		Text(ImVec2(kPadX + 16.0f, bodyY + (kHeaderH - 20.0f) * 0.5f), Type::SectionTitle,
		     theme.textPrimary, "Players");
		char count[16];
		std::snprintf(count, sizeof(count), "%u / %u", session.Count(), MAX_PLAYERS);
		TextRight(playersBox.Max().x - 16.0f, bodyY + (kHeaderH - 20.0f) * 0.5f, Type::Mono,
		          theme.textSecondary, count);

		for (uint8_t slot = 0; slot < MAX_PLAYERS; ++slot) {
			const float rowY = bodyY + kHeaderH + slot * kRowH;
			if (rowY + kRowH > playersBox.Max().y)
				break;
			draw->AddLine(ImVec2(playersBox.pos.x + 1.0f, rowY + 0.5f),
			              ImVec2(playersBox.Max().x - 1.0f, rowY + 0.5f), theme.border, 1.0f);

			const Player *p    = session.FindById(slot);
			SlotView     &view = g_state.slots[slot];
			const bool    here = p && p->active;

			if (here) {
				view.nick = p->nick;
				char activity[64];
				if (p->vehicleNetId != INVALID_NETID)
					std::snprintf(activity, sizeof(activity), "%s vehicle %u",
					              p->seat == 0 ? "Driving" : "Riding in", p->vehicleNetId);
				else
					std::snprintf(activity, sizeof(activity), "%s", p->alive ? "On foot" : "Down");
				view.activity = activity;
				view.ping     = g_state.server.PingOf(*p);
				view.shown    = true;
			}

			char slotKey[32];
			std::snprintf(slotKey, sizeof(slotKey), "##slot%u", slot);
			const float filled = Appear(slotKey, here, 10.0f);
			if (filled <= 0.0f)
				view.shown = false;

			char badge[4];
			std::snprintf(badge, sizeof(badge), "%u", slot);
			const float bw = MeasureText(Type::Mono, badge).x;
			const Rect  badgeBox{ImVec2(kPadX + 16.0f, rowY + (kRowH - 28.0f) * 0.5f),
			                     ImVec2(28.0f, 28.0f)};
			const float nameX = kPadX + 16.0f + 28.0f + 14.0f;

			// The empty row and the taken one cross over, badge included.
			if (filled < 1.0f) {
				const DrawGroup open(draw);
				draw->AddRect(badgeBox.pos, badgeBox.Max(), theme.borderControl, radius::kInput,
				              0, 1.0f);
				TextMiddle(ImVec2(badgeBox.pos.x + (28.0f - bw) * 0.5f, badgeBox.pos.y), 28.0f,
				           Type::Mono, theme.textTertiary, badge);
				TextMiddle(ImVec2(nameX, rowY), kRowH, Type::Body, theme.textTertiary, "Open");
				open.Fade(1.0f - filled);
			}

			if (!view.shown)
				continue;

			const DrawGroup taken(draw);
			draw->AddRectFilled(badgeBox.pos, badgeBox.Max(), theme.bgBadge, radius::kInput);
			TextMiddle(ImVec2(badgeBox.pos.x + (28.0f - bw) * 0.5f, badgeBox.pos.y), 28.0f,
			           Type::Mono, theme.textButton, badge);
			Text(ImVec2(nameX, rowY + 11.0f), Type::FieldLabel, theme.textPrimary,
			     view.nick.c_str());
			Text(ImVec2(nameX, rowY + 33.0f), Type::BodySmall, theme.textTertiary,
			     view.activity.c_str());

			// Ping, amber past 150 ms - the number where a session starts to
			// feel it. Eased, because ENet's round trip time moves every
			// packet and a digit that never settles is unreadable.
			char pingKey[32];
			std::snprintf(pingKey, sizeof(pingKey), "##ping%u", slot);
			const uint32_t ping = static_cast<uint32_t>(
			    Animate(pingKey, static_cast<float>(view.ping), 3.0f) + 0.5f);
			char pingText[16];
			std::snprintf(pingText, sizeof(pingText), "%u ms", ping);
			const Rect kickBox{ImVec2(playersBox.Max().x - 10.0f - 36.0f,
			                          rowY + (kRowH - 36.0f) * 0.5f),
			                   ImVec2(36.0f, 36.0f)};
			TextRight(kickBox.pos.x - 14.0f, rowY + (kRowH - 20.0f) * 0.5f, Type::Mono,
			          ping > 150 ? theme.statusWarn : theme.textSecondary, pingText);
			taken.Fade(filled);
			taken.Move(ImVec2(0.0f, (1.0f - filled) * -8.0f * Travel()));

			// Not inside the group: a button is either there to be pressed or
			// it is not, and a half-faded one that still works is worse than
			// either.
			if (here) {
				char kickId[32];
				std::snprintf(kickId, sizeof(kickId), "##kick%u", slot);
				if (IconButton(kickId, kickBox, Icon::UserX, theme, theme.textSecondary, 16.0f,
				               true, 0, theme.borderDisabled))
					g_state.server.Kick(slot);
				ImGui::SetItemTooltip("Kick %s", view.nick.c_str());
			}
		}

		// Rules. The whole card opens the options, not just the button on the
		// end of it: the three values are what somebody came here to change.
		// Submitted before the Options button, so the button still wins where
		// the two overlap.
		const float   optionsW = 110.0f;
		const Rect    rulesBox{ImVec2(rightX, bodyY), ImVec2(rightW, kRulesH)};
		const Rect    optionsBox{ImVec2(rulesBox.Max().x - 14.0f - optionsW, bodyY + 14.0f),
		                         ImVec2(optionsW, 44.0f)};
		// Stopping short of the button rather than running under it: within a
		// window ImGui gives a point to the first item submitted that claims it
		// (imgui.cpp, ItemHoverable: `if (g.HoveredId != 0 && ...) return
		// false`), so a card drawn first and covering the button would take
		// every click meant for it, and the button would never so much as
		// light up.
		const Rect    cardHit{rulesBox.pos,
		                      ImVec2(optionsBox.pos.x - 10.0f - rulesBox.pos.x, rulesBox.size.y)};
		const Touched rulesTouch = Hotspot("##rulescard", cardHit);
		if (rulesTouch.clicked) {
			g_state.editing     = g_state.config;
			g_state.optionsOpen = true;
		}
		if (rulesTouch.hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

		// A card that has just been saved glows for a moment, so a change made
		// in the dialog is visibly a change out here too.
		const float settled = ImSaturate((app.Seconds() - g_state.savedAt) / 0.9f);
		Card(rulesBox, theme, radius::kCard, Lift(theme.bgCard, theme, rulesTouch.t * 0.5f),
		     Mix(theme.statusOk, theme.border, EaseOut(settled)));
		Text(ImVec2(rightX + 20.0f, bodyY + (kRulesH - 20.0f) * 0.5f), Type::SectionTitle,
		     theme.textPrimary, "Rules");

		struct Rule {
			const char *label;
			std::string value;
		};
		const Rule rules[] = {
		    {"Friendly fire", g_state.config.friendlyFire ? "On" : "Off"},
		    {"Wanted level", Label(g_state.config.wantedLevel)},
		    {"Mission fails on death", g_state.config.missionFailOnDeath ? "On" : "Off"},
		};

		float ruleX = rightX + 20.0f + 64.0f + 22.0f;
		for (int i = 0; i < 3; ++i) {
			if (i > 0) {
				draw->AddLine(ImVec2(ruleX - 11.0f, bodyY + 14.0f),
				              ImVec2(ruleX - 11.0f, bodyY + kRulesH - 14.0f), theme.border, 1.0f);
			}
			Text(ImVec2(ruleX, bodyY + 14.0f), Type::MetaLabel, theme.textTertiary, rules[i].label);
			Text(ImVec2(ruleX, bodyY + 14.0f + 16.0f + 4.0f), Type::SectionTitle,
			     theme.textPrimary, rules[i].value.c_str());
			const float w = ImMax(MeasureText(Type::MetaLabel, rules[i].label).x,
			                      MeasureText(Type::SectionTitle, rules[i].value.c_str()).x);
			ruleX += w + 44.0f;
		}

		if (OutlineButton("##options", optionsBox, "Options", Icon::Gear, theme, true)) {
			g_state.editing     = g_state.config;
			g_state.optionsOpen = true;
		}

		// Console.
		const float consoleY = bodyY + kRulesH + 16.0f;
		const Rect  consoleBox{ImVec2(rightX, consoleY),
		                       ImVec2(rightW, screen.y - kBodyBot - consoleY)};
		Card(consoleBox, theme, radius::kCard, theme.bgConsole);
		Text(ImVec2(rightX + 16.0f, consoleY + (kHeaderH - 20.0f) * 0.5f), Type::SectionTitle,
		     theme.textPrimary, "Console");
		draw->AddLine(ImVec2(consoleBox.pos.x + 1.0f, consoleY + kHeaderH - 0.5f),
		              ImVec2(consoleBox.Max().x - 1.0f, consoleY + kHeaderH - 0.5f),
		              theme.isLight ? Rgb(0xDFE3E8) : Rgb(0x1D2229), 1.0f);

		static const char *const kFilters[] = {"All", "Players", "Chat"};
		SegmentedFlat("##filter", consoleBox.Max().x - 10.0f, consoleY + kHeaderH * 0.5f,
		              kFilters, 3, &g_state.filter, theme);

		std::vector<ConsoleLine> shown;
		shown.reserve(g_state.lines.size());
		for (const Line &line : g_state.lines) {
			if (!PassesFilter(line.kind, g_state.filter))
				continue;
			ConsoleLine out;
			out.time = line.time;
			out.tag  = TagOf(line.kind);
			out.text = line.text;
			switch (line.kind) {
			case LogKind::Join:
				out.tagColour = theme.statusOk;
				out.textColour = theme.textPrimary;
				break;
			case LogKind::Warn:
				out.tagColour  = theme.statusWarn;
				out.textColour = theme.textPrimary;
				break;
			case LogKind::Chat:
				out.tagColour  = theme.actionBg;
				out.textColour = theme.textPrimary;
				break;
			case LogKind::Detail:
				out.tagColour  = theme.textTertiary;
				out.textColour = theme.textButton;
				break;
			default:
				out.tagColour  = theme.textTertiary;
				out.textColour = theme.textSecondary;
				break;
			}
			shown.push_back(std::move(out));
		}

		const bool grew = g_state.lines.size() != g_state.lastCount;
		g_state.lastCount = g_state.lines.size();
		Console({ImVec2(consoleBox.pos.x + 1.0f, consoleY + kHeaderH),
		         ImVec2(consoleBox.size.x - 2.0f, consoleBox.size.y - kHeaderH - 1.0f)},
		        shown, theme, grew);

		ImGui::EndDisabled();

		// ---- the options dialog --------------------------------------------
		// It arrives and it leaves; while it is on its way out it is still
		// drawn but nothing in it can be pressed.
		const float dialogT = Appear("##options", g_state.optionsOpen, 17.0f);
		if (dialogT > 0.0f) {
			// The scrim starts under the title bar, as the design draws it.
			const Dialog dialog(screen, ImVec2(560.0f, 398.0f), theme, dialogT, kTitleBar);
			ImGui::BeginDisabled(!g_state.optionsOpen);

			const Rect box = dialog.box;
			Card(box, theme, radius::kDialog, theme.bgPanel, theme.borderControl);

			const float dx = box.pos.x + 28.0f;
			const float dw = box.size.x - 56.0f;
			float       dy = box.pos.y + 24.0f;

			DrawIcon(draw, Icon::Gear, ImVec2(dx, dy + 4.0f), 20.0f, theme.textSecondary);
			Text(ImVec2(dx + 30.0f, dy), Type::DialogHeading, theme.textPrimary,
			     "Server options");
			if (IconButton("##optclose", {ImVec2(box.Max().x - 28.0f - 36.0f, dy - 4.0f),
			                              ImVec2(36.0f, 36.0f)},
			               Icon::Close, theme, theme.textSecondary, 18.0f, false))
				g_state.optionsOpen = false;

			dy += 28.0f + 8.0f;
			Text(ImVec2(dx, dy), Type::Body, theme.textSecondary,
			     "Synced to every player in the session. Defaults match single player.");
			dy += 20.0f + 6.0f;

			auto row = [&](const char *title, const char *detail, float height, bool divider) {
				const float top = dy;
				dy += 18.0f;
				Text(ImVec2(dx, dy), Type::SectionTitle, theme.textPrimary, title);
				TextWrapped(ImVec2(dx, dy + 24.0f), dw - 180.0f, Type::BodySmall,
				            theme.textTertiary, detail);
				dy = top + height;
				if (divider)
					draw->AddLine(ImVec2(dx, dy - 0.5f), ImVec2(dx + dw, dy - 0.5f), theme.border,
					              1.0f);
				return top;
			};

			float top = row("Friendly fire", "Players can hurt and kill each other.", 74.0f, true);
			Switch("##optff", ImVec2(box.Max().x - 28.0f - 48.0f, top + 18.0f + 6.0f),
			       &g_state.editing.friendlyFire, theme);

			top = row("Wanted level",
			          "Per player: everyone keeps their own stars, shared while riding in the "
			          "same car.",
			          92.0f, true);
			static const char *const kWanted[] = {"Per player", "Shared", "Off"};
			int                      wanted    = static_cast<int>(g_state.editing.wantedLevel);
			const float              wantedW   = 208.0f;
			if (Segmented("##optwanted",
			              {ImVec2(box.Max().x - 28.0f - wantedW, top + 18.0f + 4.0f),
			               ImVec2(wantedW, 38.0f)},
			              kWanted, 3, &wanted, theme))
				g_state.editing.wantedLevel = static_cast<WantedLevelRule>(wanted);

			top = row("Mission fails on death",
			          "If anyone dies during a mission, it fails for everyone.", 74.0f, false);
			Switch("##optmission", ImVec2(box.Max().x - 28.0f - 48.0f, top + 18.0f + 6.0f),
			       &g_state.editing.missionFailOnDeath, theme);

			const float actionsY = box.Max().y - 24.0f - 44.0f;
			if (LinkText("##optreset", ImVec2(dx, actionsY + 13.0f), Type::ButtonSmall,
			             theme.textSecondary, "Reset to defaults"))
				g_state.editing = ServerConfig{};

			if (PrimaryButton("##optsave",
			                  {ImVec2(box.Max().x - 28.0f - 92.0f, actionsY), ImVec2(92.0f, 44.0f)},
			                  "Save", Icon::Check, true, theme, Type::ButtonSmall)) {
				const bool portChanged = g_state.editing.port != g_state.config.port;
				const bool ffChanged   = g_state.editing.friendlyFire != g_state.config.friendlyFire;
				const bool wlChanged   = g_state.editing.wantedLevel != g_state.config.wantedLevel;
				g_state.config = g_state.editing;
				g_state.config.Save(ServerConfig::Path());
				g_state.server.SessionRef().SetFriendlyFire(g_state.config.friendlyFire);
				g_state.server.SessionRef().SetAmmoSync(g_state.config.ammoSync);
				g_state.server.SessionRef().SetWantedRule(WireValue(g_state.config.wantedLevel));
				g_state.server.SessionRef().SetRampageRule(WireValue(g_state.config.rampage));
				// Not in the dialog, the same as the rampage rule: the ini is where
				// it is set, and this keeps a save from quietly putting it back.
				g_state.server.SessionRef().SetCheatRule(WireValue(g_state.config.cheats));
				g_state.server.SessionRef().SetMoneyRule(WireValue(g_state.config.money));
				g_state.server.SessionRef().SetPackageRule(
				    WireValue(g_state.config.hiddenPackages));
				g_state.savedAt = app.Seconds();
				Say(LogKind::Info, "options saved");
				if (wlChanged) {
					char line[128];
					std::snprintf(line, sizeof(line),
					              "wanted level is now %s; players already in the "
					              "session keep the old rule until they rejoin",
					              Name(g_state.config.wantedLevel));
					Say(LogKind::Info, line);
				}
				if (ffChanged)
					Say(LogKind::Info, g_state.config.friendlyFire
					                       ? "friendly fire is on; players already in the session "
					                         "are told on their next join"
					                       : "friendly fire is off");
				if (portChanged)
					Say(LogKind::Warn, "the port only changes when the server restarts");
				g_state.optionsOpen = false;
			}
			if (OutlineButton("##optcancel",
			                  {ImVec2(box.Max().x - 28.0f - 92.0f - 10.0f - 96.0f, actionsY),
			                   ImVec2(96.0f, 44.0f)},
			                  "Cancel", Icon::Close, theme, false, Type::ButtonSmall))
				g_state.optionsOpen = false;

			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				g_state.optionsOpen = false;

			ImGui::EndDisabled();
			dialog.End();
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	}, tick);

	StopServer();
	return result;
}

} // namespace coopiii
