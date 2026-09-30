// CoopIII server, the window.
//
// design/screens/Server.dc.html is the screen and ServerOptions.dc.html is the
// dialog. Every position and size below is out of those files.
//
// The server itself is server-core, the same class --nogui runs, in this same
// process. This file gives it a log sink, draws what it is doing, and lets
// somebody kick a player or change the rules.
#include "run.h"
#include <coopiii/version.h>

#include "config.h"
#include "firewall.h"
#include "options.h"
#include "probe.h"
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
#include <memory>
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
	std::deque<Line>      lines;
	bool                  running    = false;
	OptionsDialog         options;
	int                   filter     = 0;   // 0 all, 1 players, 2 chat
	size_t                lastCount  = 0;
	std::string           startError;
	bool                  copied     = false;
	float                 copiedAt   = 0.0f;
	bool                  lanCopied  = false;
	float                 lanCopiedAt = 0.0f;
	float                 tipSince   = -1.0f;    // when the pointer came onto the address
	float                 savedAt    = -10.0f;   // when the rules last changed
	SlotView              slots[MAX_PLAYERS];

	// This machine's addresses as they were when the server started, and
	// what the probe (server/probe.h) has found out about the rest. Made in
	// RunWindow rather than with the rest, so the console never starts its
	// thread.
	std::vector<LocalAddress>   local;
	std::unique_ptr<ReachProbe> probe;
};

State g_state;

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
	// Everything else in the ini: the password, the player limit, the mission
	// rules and the votes. SaveOptions calls it again with whatever changed.
	g_state.server.Configure(g_state.config);
	g_state.local = LocalIPv4Addresses();
	for (const std::string &line :
	     ReachLines(g_state.local, g_state.config.port, g_state.config.lookUpPublicAddress))
		Say(LogKind::Info, line.c_str());
	g_state.probe->Begin(g_state.config.port, g_state.local, g_state.config.lookUpPublicAddress,
	                     g_state.config.openRouterPort);
}

void StopServer() {
	if (!g_state.running)
		return;
	g_state.server.Stop();
	g_state.probe->End();
	g_state.running = false;
	Say(LogKind::Info, "stopped");
}

// The colour of the icon in front of a line under the address.
ImU32 ToneColour(Tone tone, const Theme &theme) {
	return tone == Tone::Warn ? theme.statusWarn : tone == Tone::Ok ? theme.statusOk
	                                                                : theme.textTertiary;
}

// One of the lines under the address, right-aligned like the design's hint,
// with an icon in front when it is good news or something to act on.
void HintLine(ImDrawList *draw, float rightEdge, float y, const Advice &advice,
              const Theme &theme) {
	if (advice.text.empty())
		return;
	const float w = MeasureText(Type::Hint, advice.text.c_str()).x;
	TextRight(rightEdge, y, Type::Hint,
	          advice.tone == Tone::Info ? theme.textTertiary : theme.textSecondary,
	          advice.text.c_str());
	if (advice.tone != Tone::Info)
		DrawIcon(draw, advice.tone == Tone::Ok ? Icon::CircleCheck : Icon::TriangleAlert,
		         ImVec2(rightEdge - w - 18.0f, y + 1.5f), 13.0f, ToneColour(advice.tone, theme));
}

// The dialog's Save: the file first, then the running session, then a line
// in the console for each setting that moved, so what changed and when is
// there to read back.
void SaveOptions(float now) {
	const ServerConfig before = g_state.config;
	g_state.config            = g_state.options.editing;
	const std::string path    = ServerConfig::Path();
	if (!g_state.config.Save(path))
		Say(LogKind::Warn, ("could not write " + path + "; the options hold until the server "
		                    "closes")
		                       .c_str());
	if (g_state.running)
		g_state.server.Configure(g_state.config);
	g_state.savedAt = now;

	const std::vector<std::string> changes = DescribeChanges(before, g_state.config);
	if (changes.empty()) {
		Say(LogKind::Info, "options saved, nothing changed");
		return;
	}
	Say(LogKind::Info, "options saved");
	for (const std::string &line : changes)
		Say(LogKind::Info, line.c_str());
	if (g_state.config.port != before.port && g_state.running)
		Say(LogKind::Warn, "the port changes when the server is stopped and started again");
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
	g_state.server.SetProgressPath(ProgressPath());
	if (startup.forgetProgress)
		g_state.server.ForgetProgress();
	g_state.probe = std::make_unique<ReachProbe>();
	StartServer();

	// Servicing the session is not part of drawing. A dedicated server spends
	// its life minimised, or behind something, or being dragged across a
	// screen - and App::Run skips the frame for all three. This runs every
	// turn of the loop regardless. 0 so it never blocks. What the probe has
	// found out goes into the console here too, so none of it waits for the
	// window to be looked at.
	const auto tick = [] {
		if (g_state.running)
			g_state.server.Tick(0);
		for (const ProbeLine &line : g_state.probe->TakeLines())
			Say(line.warn ? LogKind::Warn : LogKind::Info, line.text.c_str());
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

		const TitleBarResult bar = TitleBar(app, "Server", "v" COOPIII_VERSION, true);
		if (bar.minimise) app.Minimize();
		if (bar.maximise) app.ToggleMaximize();
		if (bar.close)    app.Close();
		if (bar.theme)    app.SetTheme(!app.IsLight());

		// While the options are up, the screen behind them is a picture. The
		// scrim says as much and this makes it true. The title bar is outside
		// it on purpose: a dialog should never be able to trap the window.
		ImGui::BeginDisabled(g_state.options.open);

		const float titleY = bandY + kBandTop;
		const float titleW = Text(ImVec2(kPadX, titleY), Type::WindowHeading, theme.textPrimary,
		                          "Server");
		const float pillW =
		    g_state.running
		        ? Pill(ImVec2(kPadX + titleW + 14.0f, titleY + 4.0f), "Running", theme.statusOk,
		               theme.okPillBg, theme.okPillBorder, true)
		        : Pill(ImVec2(kPadX + titleW + 14.0f, titleY + 4.0f), "Stopped",
		               theme.textTertiary, theme.bgSubtle, theme.borderControl);

		char summary[256];
		if (g_state.running)
			std::snprintf(summary, sizeof(summary),
			              "Listening on UDP port %u \xC2\xB7 %u slots \xC2\xB7 protocol v%u "
			              "\xC2\xB7 up %s \xC2\xB7 in-game %02u:%02u",
			              g_state.server.Port(), g_state.server.PlayerLimit(), PROTOCOL_VERSION,
			              Clock(g_state.server.UptimeMs()).c_str(), session.Clock().Hour(),
			              session.Clock().Minute());
		else
			std::snprintf(summary, sizeof(summary), "%s",
			              g_state.startError.empty()
			                  ? "Not listening. Press Start server to open the port again."
			                  : g_state.startError.c_str());
		const float summaryW =
		    Text(ImVec2(kPadX, titleY + 34.0f + 6.0f), Type::BodySmall,
		         g_state.startError.empty() ? theme.textSecondary : theme.statusFail, summary);
		// Where the left of the band ends, which the address cards stay clear of.
		const float leftEnd = ImMax(kPadX + titleW + 14.0f + pillW, kPadX + summaryW) + 20.0f;

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

		// The address to hand out: the public one once it is known, since that
		// is the one a friend on the internet types, with the same network's
		// in a smaller card beside it (reach.h, ShareFor). Until then, or with
		// the lookup off or failed, this network's, as it always was.
		const uint16_t  port     = g_state.server.Port();
		const ShareView share    = ShareFor(g_state.local, g_state.probe->Status(), port);
		const bool      asking   = share.address.empty();
		const char     *addrText = asking ? "Looking up..." : share.address.c_str();

		const float addrTextW = MeasureText(Type::MonoLarge, addrText).x;
		const float addrW     = 14.0f + ImMax(addrTextW, 112.0f) + 16.0f + 36.0f + 8.0f;
		const Rect  addrBox{ImVec2(stopBox.pos.x - 12.0f - addrW, titleY), ImVec2(addrW, 56.0f)};
		Card(addrBox, theme, radius::kPrimary);
		Text(Add(addrBox.pos, ImVec2(14.0f, 8.0f)), Type::MetaLabel, theme.textTertiary,
		     share.label);
		Text(Add(addrBox.pos, ImVec2(14.0f, 8.0f + 16.0f + 2.0f)), Type::MonoLarge,
		     asking ? theme.textTertiary : theme.textPrimary, addrText);
		// The tick that says it went on the clipboard grows in and the copy
		// icon fades under it, then they swap back a second and a half later.
		const Rect  copyBox{ImVec2(addrBox.Max().x - 8.0f - 36.0f, addrBox.pos.y + 10.0f),
		                    ImVec2(36.0f, 36.0f)};
		const float done = Appear("##copied", g_state.copied, 18.0f);
		ImGui::BeginDisabled(asking);
		if (IconButton("##copy", copyBox, Icon::Copy, theme,
		               WithAlpha(asking ? theme.textMuted : theme.textButton, 1.0f - done), 16.0f,
		               true, theme.bgSubtle)) {
			App::SetClipboard(share.address);
			g_state.copied   = true;
			g_state.copiedAt = app.Seconds();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Copy");
		if (done > 0.0f) {
			const DrawGroup tick(draw);
			DrawIcon(draw, Icon::Check, ImVec2(copyBox.pos.x + 10.0f, copyBox.pos.y + 10.0f),
			         16.0f, theme.statusOk);
			tick.Fade(done);
			tick.Scale(copyBox.Centre(), 0.5f + 0.5f * EaseBack(done));
		}
		if (g_state.copied && app.Seconds() - g_state.copiedAt > 1.6f)
			g_state.copied = false;

		// Resting on the card says who the address is for. Tested rather
		// than submitted, so the copy button keeps its clicks, and after a
		// moment, so crossing the header does not flash it.
		const bool overAddr =
		    !g_state.options.open && ImGui::IsMouseHoveringRect(addrBox.pos, addrBox.Max()) &&
		    !ImGui::IsMouseHoveringRect(copyBox.pos, copyBox.Max());
		if (!overAddr)
			g_state.tipSince = -1.0f;
		else if (g_state.tipSince < 0.0f)
			g_state.tipSince = app.Seconds();
		else if (app.Seconds() - g_state.tipSince > 0.4f) {
			const ReachStatus status = g_state.probe->Status();
			if (share.isPublic)
				ImGui::SetTooltip("What friends on the internet type to join.\nOn this PC, "
				                  "connect with 127.0.0.1:%u instead: many routers do not loop\n"
				                  "the public address back to your own network.",
				                  static_cast<unsigned>(port));
			else if (asking)
				ImGui::SetTooltip("Asking a what-is-my-IP service for this network's public "
				                  "address.");
			else
				ImGui::SetTooltip("What players on your own network type to join.%s",
				                  status.lookup == LookupState::Off
				                      ? "\nLooking up the public address is off in Options."
				                  : status.lookup == LookupState::Failed
				                      ? "\nThe public address could not be looked up."
				                      : "");
		}

		// The same network's, when the main card is the public one and there
		// is room beside the band's text. Clicking anywhere on it copies it.
		if (!share.lan.empty()) {
			const float lanTextW = MeasureText(Type::Mono, share.lan.c_str()).x;
			const float lanW     = 14.0f + ImMax(lanTextW, 84.0f) + 16.0f;
			const Rect  lanBox{ImVec2(addrBox.pos.x - 10.0f - lanW, titleY), ImVec2(lanW, 56.0f)};
			if (lanBox.pos.x >= leftEnd) {
				const Touched lanTouch = Hotspot("##lancard", lanBox);
				if (lanTouch.clicked) {
					App::SetClipboard(share.lan);
					g_state.lanCopied   = true;
					g_state.lanCopiedAt = app.Seconds();
				}
				if (lanTouch.hovered) {
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					ImGui::SetTooltip("What players on your own network type. Click to copy.");
				}
				if (g_state.lanCopied && app.Seconds() - g_state.lanCopiedAt > 1.6f)
					g_state.lanCopied = false;
				const float lanDone = Appear("##lancopied", g_state.lanCopied, 18.0f);
				Card(lanBox, theme, radius::kPrimary, Lift(theme.bgCard, theme, lanTouch.t * 0.5f));
				const ImVec2 labelAt = Add(lanBox.pos, ImVec2(14.0f, 8.0f));
				{
					const DrawGroup label(draw);
					Text(labelAt, Type::MetaLabel, theme.textTertiary, "Same network");
					label.Fade(1.0f - lanDone);
				}
				if (lanDone > 0.0f) {
					const DrawGroup label(draw);
					Text(labelAt, Type::MetaLabel, theme.statusOk, "Copied");
					label.Fade(lanDone);
				}
				Text(Add(lanBox.pos, ImVec2(14.0f, 8.0f + 16.0f + 4.0f)), Type::Mono,
				     theme.textSecondary, share.lan.c_str());
			}
		}

		// Under them, the router and how the host connects - or, stopped,
		// nothing, since none of it is true of a server that is not there.
		if (g_state.running) {
			HintLine(draw, rightEdge, titleY + 56.0f + 8.0f, share.port, theme);
			HintLine(draw, rightEdge, titleY + 56.0f + 8.0f + 17.0f, share.note, theme);
		}

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
		std::snprintf(count, sizeof(count), "%u / %u", session.Count(), 
		              g_state.server.PlayerLimit());
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
				// Closed rather than open once the server's player limit is
				// reached: nobody else can take it.
				const bool closed = session.Count() >= g_state.server.PlayerLimit();
				TextMiddle(ImVec2(nameX, rowY), kRowH, Type::Body,
				           closed ? theme.textMuted : theme.textTertiary, closed ? "Closed" : "Open");
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
		// end of it: the values are what somebody came here to change.
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
			g_state.options.Open(g_state.config);
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

		// As many of the rules as fit before the button, in the order people
		// ask about them (options.cpp, SummariseRules). The rest are a click
		// away, and the tooltip says how many.
		const std::vector<RuleSummary> rules = SummariseRules(g_state.config);
		float ruleX = rightX + 20.0f + 64.0f + 22.0f;
		size_t ruleCount = 0;
		for (size_t i = 0; i < rules.size(); ++i) {
			const float w = ImMax(MeasureText(Type::MetaLabel, rules[i].label).x,
			                      MeasureText(Type::SectionTitle, rules[i].value.c_str()).x);
			if (ruleX + w > optionsBox.pos.x - 16.0f)
				break;
			++ruleCount;
			if (i > 0) {
				draw->AddLine(ImVec2(ruleX - 11.0f, bodyY + 14.0f),
				              ImVec2(ruleX - 11.0f, bodyY + kRulesH - 14.0f), theme.border, 1.0f);
			}
			Text(ImVec2(ruleX, bodyY + 14.0f), Type::MetaLabel, theme.textTertiary, rules[i].label);
			Text(ImVec2(ruleX, bodyY + 14.0f + 16.0f + 4.0f), Type::SectionTitle,
			     theme.textPrimary, rules[i].value.c_str());
			ruleX += w + 44.0f;
		}
		if (rulesTouch.hovered && ruleCount < SettingCount())
			ImGui::SetTooltip("%zu more settings in Options", SettingCount() - ruleCount);

		if (OutlineButton("##options", optionsBox, "Options", Icon::Gear, theme, true)) {
			g_state.options.Open(g_state.config);
		}

		// Windows Firewall, when it is keeping players out (firewall.h). Only
		// then: a firewall that lets the server in is a line in the console,
		// not something to look at all session. The console gives up the room.
		float consoleY = bodyY + kRulesH + 16.0f;
		{
			const FirewallVerdict verdict = g_state.probe->Firewall();
			const FirewallAdvice  advice  = AdviseFirewall(verdict, g_state.server.Port());
			const bool            warn    = g_state.running && advice.tone == Tone::Warn;
			const float           shown   = Appear("##firewall", warn, 12.0f);
			if (shown > 0.0f) {
				const bool  allowing = g_state.probe->Allowing();
				const float buttonW  = advice.canFix ? 204.0f : 0.0f;
				const float textX    = rightX + 16.0f + 18.0f + 12.0f;
				const float textW =
				    rightW - (textX - rightX) - 16.0f - (buttonW > 0.0f ? buttonW + 16.0f : 0.0f);
				const float detailH = MeasureWrapped(Type::BodySmall, textW, advice.detail.c_str()).y;
				const float bannerH = ImMax(68.0f, 11.0f + 20.0f + detailH + 12.0f);
				const Rect  banner{ImVec2(rightX, consoleY), ImVec2(rightW, bannerH)};
				{
					const DrawGroup group(draw);
					Card(banner, theme, radius::kCard, theme.bgCard,
					     Mix(theme.border, theme.statusWarn, 0.55f));
					DrawIcon(draw, Icon::TriangleAlert, ImVec2(rightX + 16.0f, consoleY + 13.0f),
					         18.0f, theme.statusWarn);
					Text(ImVec2(textX, consoleY + 11.0f), Type::SectionTitle, theme.textPrimary,
					     advice.title.c_str());
					TextWrapped(ImVec2(textX, consoleY + 11.0f + 20.0f), textW, Type::BodySmall,
					            theme.textTertiary, advice.detail.c_str());
					group.Fade(shown);
					group.Move(ImVec2(0.0f, (1.0f - shown) * -6.0f * Travel()));
				}
				// The button is there to be pressed or it is not (see the kick
				// button); it comes once the banner has.
				if (advice.canFix && warn && shown > 0.9f) {
					const Rect allowBox{ImVec2(banner.Max().x - 12.0f - buttonW,
					                           consoleY + (bannerH - 44.0f) * 0.5f),
					                    ImVec2(buttonW, 44.0f)};
					ImGui::BeginDisabled(allowing);
					if (OutlineButton("##fwallow", allowBox,
					                  allowing ? "Waiting for Windows" : "Allow in firewall",
					                  Icon::ShieldCheck, theme, false, Type::ButtonSmall,
					                  theme.statusOk))
						g_state.probe->AllowInFirewall(app.Hwnd());
					ImGui::EndDisabled();
					ImGui::SetItemTooltip(
					    "Adds a Windows Firewall rule letting server.exe receive UDP, with netsh "
					    "run as administrator.%s\nWindows asks for permission first; nothing "
					    "changes if you say no.",
					    verdict.state == FirewallState::Blocked
					        ? "\nserver.exe's other inbound rules are deleted first, the one "
					          "blocking it among them,\nsince a block beats any allow."
					        : "");
				}
				consoleY += (bannerH + 16.0f) * EaseOut(shown);
			}
		}

		// Console.
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
		// options.cpp. It arrives and it leaves; while it is on its way out it
		// is still drawn but nothing in it can be pressed.
		switch (DrawOptions(g_state.options, g_state.config, app, screen, kTitleBar)) {
		case OptionsResult::Save:
			SaveOptions(app.Seconds());
			break;
		case OptionsResult::ForgetProgress:
			// Refused, and said so in the console, while anybody is in.
			g_state.server.ForgetProgress();
			break;
		default:
			break;
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	}, tick);

	StopServer();
	// Closes the port on the router, if it was opened, before the process
	// goes. Bounded, so a router that has stopped answering cannot keep it.
	g_state.probe->Finish();
	return result;
}

} // namespace coopiii
