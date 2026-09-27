// CoopIII launcher, the window.
//
// design/screens/Main.dc.html is the screen when everything is in place, and
// LauncherBlocked.dc.html is the same screen with problems on it. Every
// position and size below is out of those files, but for the lobby's column
// (docs/protocol.md 1.31), which has no screen of its own yet and is built
// out of the same parts: the checks' card for the list, the options dialog's
// segmented control and the start button where it always was.
//
// All the thinking lives in launcher-core, which the console launcher uses
// too. This file is layout, and the one decision the window makes that the
// CLI does not: it writes CoopIII.ini before starting, so the name and server
// typed here are the ones the client reads.
#include "run.h"

#include "launcher/core.h"
#include "launcher/lobby.h"

#include "ui/anim.h"
#include "ui/app.h"
#include "ui/citymap.h"
#include "ui/widgets.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace ui;

namespace coopiii::launcher {
namespace {

// The design's window, to the pixel.
constexpr float kWindowW   = 1040.0f;
constexpr float kWindowH   = 700.0f;
constexpr float kTitleBar  = 40.0f;
constexpr float kLeftW     = 440.0f;
constexpr float kLeftPad   = 40.0f;
constexpr float kLeftBottom = 32.0f;
constexpr float kRightPadX = 48.0f;
constexpr float kRightPadT = 44.0f;
constexpr float kRightPadB = 32.0f;
constexpr float kLogoW     = 216.0f;

struct State {
	char nick[64]    = "Player";
	char host[128]   = "127.0.0.1";
	char port[8]     = "2001";
	char gameDir[MAX_PATH] = "";

	Checks      checks;
	std::string startError;
	bool        launched = false;
	float       checkedAt = -10.0f;   // when the list was last re-run

	// The lobby (docs/protocol.md 1.31): the right column shows it instead
	// of the form while this is set.
	LobbyClient lobby;
	bool        lobbyView = false;
	int         startMode = 0;   // the host's choice: 0 a new game, 1 the menu
};

State g_state;

// ---- the check list -------------------------------------------------------

float DetailHeight(const Check &check, float width) {
	if (check.detail.empty())
		return 0.0f;
	return MeasureWrapped(Type::BodySmall, width, check.detail.c_str()).y;
}

// A passing row is 44 tall; a failing one grows for its fix text and, on the
// version check, the MD5 underneath.
float RowHeight(const Check &check, float textWidth) {
	if (check.state != Check::State::Fail && check.detail.empty())
		return 44.0f;

	float h = 12.0f + 20.0f;                       // padding-top + the title line
	h += 4.0f + DetailHeight(check, textWidth);    // gap + the fix text
	if (!check.mono.empty())
		h += 4.0f + 16.0f + 2.0f + 16.0f;          // margin-top + label + gap + value
	return h + 12.0f;                              // padding-bottom
}

// A row's height eased toward what it should be. A check that starts failing
// grows its fix text out rather than teleporting it, and because the list is
// anchored to the bottom of the column the whole block slides with it.
float ShownRowHeight(size_t index, const Check &check, float textWidth) {
	char key[32];
	std::snprintf(key, sizeof(key), "##rowh%zu", index);
	return Animate(key, RowHeight(check, textWidth), 15.0f);
}

float ListHeight(const Checks &checks, float width) {
	const float textWidth = width - 14.0f - 18.0f - 12.0f - 14.0f;
	float       h         = 2.0f;   // the card's own top and bottom border
	for (size_t i = 0; i < checks.items.size(); ++i) {
		h += ShownRowHeight(i, checks.items[i], textWidth);
		if (i + 1 < checks.items.size())
			h += 1.0f;   // the divider between rows
	}
	return h;
}

void DrawCheckList(Rect box, const Checks &checks, const Theme &theme) {
	Card(box, theme);

	ImDrawList *draw      = ImGui::GetWindowDrawList();
	const float textX     = box.pos.x + 14.0f + 18.0f + 12.0f;
	const float textWidth = box.size.x - 14.0f - 18.0f - 12.0f - 14.0f;
	float       y         = box.pos.y + 1.0f;

	for (size_t i = 0; i < checks.items.size(); ++i) {
		const Check &check  = checks.items[i];
		const float  height = ShownRowHeight(i, check, textWidth);
		const bool   tall   = RowHeight(check, textWidth) > 44.0f;

		// Nothing below this row should see what a shrinking one is still
		// drawing.
		draw->PushClipRect(ImVec2(box.pos.x + 1.0f, y),
		                   ImVec2(box.Max().x - 1.0f, y + height), true);

		ImU32 iconColour = theme.statusOk;
		Icon  icon       = Icon::CircleCheck;
		if (check.state == Check::State::Fail) {
			iconColour = theme.statusFail;
			icon       = Icon::CircleX;
		} else if (check.state == Check::State::Info) {
			iconColour = theme.textTertiary;
			icon       = Icon::Info;
		}

		DrawIcon(draw, icon,
		         ImVec2(box.pos.x + 14.0f, tall ? y + 13.0f : y + (44.0f - 18.0f) * 0.5f),
		         18.0f, iconColour);

		if (tall) {
			Text(ImVec2(textX, y + 12.0f), Type::FieldLabel, theme.textPrimary,
			     check.title.c_str());
			float below = y + 12.0f + 20.0f + 4.0f;
			if (!check.detail.empty())
				below += TextWrapped(ImVec2(textX, below), textWidth, Type::BodySmall,
				                     theme.textSecondary, check.detail.c_str());
			if (!check.mono.empty()) {
				below += 4.0f;
				Text(ImVec2(textX, below), Type::Hint, theme.textTertiary,
				     check.monoLabel.c_str());
				Text(ImVec2(textX, below + 18.0f), Type::Hint, theme.textButton,
				     check.mono.c_str());
			}
			// The one link in the design: Ultimate ASI Loader's releases page.
			if (!check.link.empty()) {
				char id[64];
				std::snprintf(id, sizeof(id), "##link%zu", i);
				const Rect hit{ImVec2(textX, y + 12.0f + 20.0f),
				               ImVec2(textWidth, DetailHeight(check, textWidth))};
				bool hovered = false;
				ImGui::SetCursorScreenPos(hit.pos);
				ImGui::InvisibleButton(id, hit.size);
				hovered = ImGui::IsItemHovered();
				if (hovered)
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (ImGui::IsItemClicked())
					ShellExecuteA(nullptr, "open", check.link.c_str(), nullptr, nullptr,
					              SW_SHOWNORMAL);
				ImGui::SetItemTooltip("%s", check.link.c_str());
			}
		} else {
			const ImU32 colour =
			    check.state == Check::State::Info ? theme.textSecondary : theme.textPrimary;
			TextMiddle(ImVec2(textX, y), 44.0f, Type::Body, colour, check.title.c_str());
			if (!check.meta.empty())
				TextRight(box.Max().x - 14.0f, y + (44.0f - 16.0f) * 0.5f, Type::Hint,
				          theme.textTertiary, check.meta.c_str());
		}

		draw->PopClipRect();

		y += height;
		if (i + 1 < checks.items.size()) {
			draw->AddLine(ImVec2(box.pos.x + 1.0f, y + 0.5f),
			              ImVec2(box.Max().x - 1.0f, y + 0.5f), theme.border, 1.0f);
			y += 1.0f;
		}
	}
}

// ---- state ----------------------------------------------------------------

void CopyInto(char *dst, size_t size, const std::string &src) {
	std::snprintf(dst, size, "%s", src.c_str());
}

void Recheck() {
	g_state.checks = RunChecks(g_state.gameDir);
	g_state.startError.clear();
}

// The command line first, then the game it finds and what its CoopIII.ini
// says, then what the command line said about the server again - an argument
// is somebody asking for something on purpose, and the ini is only last time.
void FindGameAndLoadConfig(const Startup &startup) {
	const std::string found = startup.gameDir.empty() ? FindGameDir() : startup.gameDir;
	CopyInto(g_state.gameDir, sizeof(g_state.gameDir), found);

	if (!found.empty()) {
		std::string host = g_state.host;
		std::string nick = g_state.nick;
		uint16_t    port = 2001;
		ReadIni(Join(found, "CoopIII.ini"), &host, &port, &nick);
		CopyInto(g_state.host, sizeof(g_state.host), host);
		CopyInto(g_state.nick, sizeof(g_state.nick), SanitizeNick(nick));
		std::snprintf(g_state.port, sizeof(g_state.port), "%u", port);
	}

	if (!startup.host.empty())
		CopyInto(g_state.host, sizeof(g_state.host), startup.host);
	if (startup.port != 0)
		std::snprintf(g_state.port, sizeof(g_state.port), "%u", startup.port);
	if (!startup.nick.empty())
		CopyInto(g_state.nick, sizeof(g_state.nick), SanitizeNick(startup.nick));
	Recheck();
}

bool FormIsValid(uint16_t *port) {
	return SanitizeNick(g_state.nick).size() > 0 && ValidHost(g_state.host) &&
	       ParsePort(g_state.port, port);
}

// ---- starting -------------------------------------------------------------

uint32_t NowMs() { return static_cast<uint32_t>(GetTickCount()); }

// Writes the form into CoopIII.ini and starts the game: alone, or because the
// lobby's host started everybody's, into a new game with `newGame`. The
// window closes behind a game that started.
bool StartGame(bool newGame) {
	uint16_t port = 0;
	if (!FormIsValid(&port)) {
		g_state.startError = "Fill in your name, the server address and the port.";
		return false;
	}
	const std::string nick = SanitizeNick(g_state.nick);
	CopyInto(g_state.nick, sizeof(g_state.nick), nick);
	g_state.startError.clear();
	if (!UpdateIni(Join(g_state.gameDir, "CoopIII.ini"), g_state.host, port, nick)) {
		g_state.startError = "Could not write CoopIII.ini in the game folder.";
		return false;
	}
	if (!LaunchGame(g_state.gameDir, &g_state.startError, newGame))
		return false;   // LaunchGame filled in why
	g_state.launched = true;
	return true;
}

// Into the server's lobby with what the form says, and the password
// CoopIII.ini has, if any.
void JoinLobby(uint16_t port) {
	const std::string nick = SanitizeNick(g_state.nick);
	CopyInto(g_state.nick, sizeof(g_state.nick), nick);
	std::string password;
	ReadIni(Join(g_state.gameDir, "CoopIII.ini"), nullptr, nullptr, nullptr, &password);
	g_state.startError.clear();
	g_state.lobbyView = true;
	g_state.lobby.Join(g_state.host, port, nick, password, NowMs());
}

// ---- the lobby ------------------------------------------------------------

constexpr float kLobbyRowH = 40.0f;

// Who is waiting and who is playing, in a card like the checks'. As many rows
// as fit in `maxH`, the last one saying how many more there are.
float DrawPeople(Rect area, const LobbyClient &lobby, const Theme &theme) {
	const std::vector<LobbyClient::Person> &people = lobby.People();
	if (people.empty())
		return 0.0f;
	size_t rows = people.size();
	const size_t fit = static_cast<size_t>((area.size.y - 2.0f) / (kLobbyRowH + 1.0f));
	const bool   more = rows > fit && fit >= 1;
	if (rows > fit)
		rows = fit;
	if (rows == 0)
		return 0.0f;
	const float h = 2.0f + rows * kLobbyRowH + (rows - 1) * 1.0f;
	const Rect  box{area.pos, ImVec2(area.size.x, h)};
	Card(box, theme);

	ImDrawList *draw = ImGui::GetWindowDrawList();
	float       y    = box.pos.y + 1.0f;
	for (size_t i = 0; i < rows; ++i) {
		if (more && i + 1 == rows) {
			char left[48];
			std::snprintf(left, sizeof(left), "and %zu more", people.size() - i);
			TextMiddle(ImVec2(box.pos.x + 34.0f, y), kLobbyRowH, Type::BodySmall,
			           theme.textTertiary, left);
			break;
		}
		const LobbyClient::Person &p = people[i];
		const ImU32 dot = p.playing ? theme.statusOk : p.host ? theme.brandRed : theme.actionBg;
		draw->AddCircleFilled(ImVec2(box.pos.x + 18.0f, y + kLobbyRowH * 0.5f), 4.0f, dot);
		TextMiddle(ImVec2(box.pos.x + 34.0f, y), kLobbyRowH, Type::FieldLabel, theme.textPrimary,
		           p.nick.empty() ? "?" : p.nick.c_str());
		const char *what = p.playing ? "Playing"
		                   : p.host && p.you ? "Host \xC2\xB7 you"
		                   : p.host        ? "Host"
		                   : p.you         ? "Waiting \xC2\xB7 you"
		                                   : "Waiting";
		TextRight(box.Max().x - 14.0f, y + (kLobbyRowH - 16.0f) * 0.5f, Type::Hint,
		          theme.textTertiary, what);
		y += kLobbyRowH;
		if (i + 1 < rows) {
			draw->AddLine(ImVec2(box.pos.x + 1.0f, y + 0.5f), ImVec2(box.Max().x - 1.0f, y + 0.5f),
			              theme.border, 1.0f);
			y += 1.0f;
		}
	}
	return h;
}

// The right column while in the lobby: who is there, and for its host the
// start of everybody's game.
// The lobby's network and the host's start, every turn of the window's loop,
// minimised or not: somebody waiting with the window down still starts with
// everybody else. True when this game started.
bool ServiceLobby() {
	if (!g_state.lobbyView)
		return false;
	LobbyClient &lobby = g_state.lobby;
	lobby.Service(NowMs());
	uint8_t     mode = 0;
	std::string by;
	return lobby.TakeStart(&mode, &by) && StartGame(mode == LOBBY_START_NEW_GAME);
}

void DrawLobby(const Theme &theme, float rightX, float rightW, float top, float bottom) {
	LobbyClient &lobby = g_state.lobby;

	using Phase       = LobbyClient::Phase;
	const Phase phase = lobby.GetPhase();
	const bool  in    = phase == Phase::In;
	const bool  trying = phase == Phase::Connecting || phase == Phase::Asking;

	float       y     = top;
	const float headW = Text(ImVec2(rightX, y), Type::WindowHeading, theme.textPrimary, "Lobby");
	if (in)
		Pill(ImVec2(rightX + headW + 14.0f, y + 4.0f), "Waiting", theme.statusOk, theme.okPillBg,
		     theme.okPillBorder, true);
	else if (trying)
		Pill(ImVec2(rightX + headW + 14.0f, y + 4.0f), "Connecting", theme.textTertiary,
		     theme.bgSubtle, theme.borderControl, true);
	else
		Pill(ImVec2(rightX + headW + 14.0f, y + 4.0f),
		     phase == Phase::Refused ? "Turned away" : "Not connected", theme.statusFail,
		     theme.bgSubtle, theme.borderControl);
	y += 34.0f + 10.0f;

	char where[256];
	if (in)
		std::snprintf(where, sizeof(where), "%s:%s as %s \xC2\xB7 %zu waiting \xC2\xB7 %zu playing",
		              g_state.host, g_state.port, g_state.nick, lobby.Waiting(), lobby.Playing());
	else
		std::snprintf(where, sizeof(where), "%s:%s as %s", g_state.host, g_state.port,
		              g_state.nick);
	y += TextWrapped(ImVec2(rightX, y), rightW, Type::Body, theme.textSecondary, where);
	y += 24.0f;

	// The bottom block: the host's choice and the button, then one line.
	const float buttonY = bottom - 18.0f - 10.0f - 60.0f;
	const float choiceY = buttonY - 14.0f - 36.0f - 8.0f - 40.0f;
	const bool  host    = in && lobby.WeAreHost();

	if (in) {
		const float listBottom = (host ? choiceY : buttonY) - 20.0f;
		DrawPeople({ImVec2(rightX, y), ImVec2(rightW, listBottom - y)}, lobby, theme);
	} else if (trying) {
		TextWrapped(ImVec2(rightX, y), rightW, Type::Body, theme.textTertiary,
		            "Getting into the server's lobby...");
	} else {
		TextWrapped(ImVec2(rightX, y), rightW, Type::Body, theme.statusFail,
		            lobby.Problem().c_str());
	}

	if (host) {
		static const char *const kModes[] = {"New game", "Load a save"};
		Segmented("##lobbymode", {ImVec2(rightX, choiceY), ImVec2(rightW, 40.0f)}, kModes, 2,
		          &g_state.startMode, theme);
		TextWrapped(ImVec2(rightX, choiceY + 40.0f + 8.0f), rightW, Type::BodySmall,
		            theme.textTertiary,
		            g_state.startMode == 0
		                ? "Everybody's game goes past the menu into a new game. The first mission "
		                  "waits at the bridge until everybody's intro is over."
		                : "Everybody's game opens at the menu, where each player loads their own "
		                  "save.");
	}

	const bool ready = g_state.checks.Ready();
	if (host) {
		if (PrimaryButton("##lobbystart", {ImVec2(rightX, buttonY), ImVec2(rightW, 60.0f)},
		                  "Start everybody's game", Icon::Play, ready, theme))
			lobby.Start(g_state.startMode == 0 ? LOBBY_START_NEW_GAME : LOBBY_START_MENU, NowMs());
	} else if (!in && !trying) {
		uint16_t port = 0;
		if (PrimaryButton("##lobbyretry", {ImVec2(rightX, buttonY), ImVec2(rightW, 60.0f)},
		                  "Join the lobby again", Icon::RotateCcw, FormIsValid(&port), theme))
			JoinLobby(port);
	} else {
		char label[96];
		const std::string hostNick = lobby.HostNick();
		if (in && !hostNick.empty())
			std::snprintf(label, sizeof(label), "Waiting for %s", hostNick.c_str());
		else
			std::snprintf(label, sizeof(label), "Waiting");
		PrimaryButton("##lobbywait", {ImVec2(rightX, buttonY), ImVec2(rightW, 60.0f)}, label,
		              Icon::Users, false, theme);
	}

	// One line under the button: what is going on on the left, the ways out
	// on the right.
	const float noteY  = buttonY + 60.0f + 10.0f;
	const char *leave  = "Leave the lobby";
	const char *alone  = "Start on my own";
	const float leaveW = MeasureText(Type::BodySmall, leave).x;
	const float aloneW = MeasureText(Type::BodySmall, alone).x;
	const float linksX = rightX + rightW - leaveW - 20.0f - aloneW;
	if (LinkText("##lobbyleave", ImVec2(rightX + rightW - leaveW, noteY), Type::BodySmall,
	             theme.textButton, leave)) {
		lobby.Leave();
		g_state.lobbyView = false;
	}
	if (ready && LinkText("##lobbyalone", ImVec2(linksX, noteY), Type::BodySmall,
	                      theme.textButton, alone))
		StartGame(false);

	char note[160];
	const char *text = note;
	ImU32       colour = theme.textTertiary;
	if (!g_state.startError.empty()) {
		text   = g_state.startError.c_str();
		colour = theme.statusFail;
	} else if (!ready) {
		text   = "Fix the problems on the left first.";
		colour = theme.textSecondary;
	} else if (host) {
		text = "You start everybody's game.";
	} else if (in) {
		std::snprintf(note, sizeof(note), "%s starts everybody's game.",
		              lobby.HostNick().empty() ? "The host" : lobby.HostNick().c_str());
	} else {
		text = "";
	}
	ImDrawList *draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(ImVec2(rightX, noteY), ImVec2(linksX - 16.0f, noteY + 20.0f), true);
	Text(ImVec2(rightX, noteY), Type::BodySmall, colour, text);
	draw->PopClipRect();
}

} // namespace

int RunWindow(const Startup &startup) {
	AppOptions options;
	options.title          = "CoopIII Launcher";
	options.width          = static_cast<int>(kWindowW);
	options.height         = static_cast<int>(kWindowH);
	options.resizable      = false;
	options.maximizeBox    = false;
	options.titleBarHeight = kTitleBar;

	App app(options);
	if (!app.Ok()) {
		MessageBoxW(nullptr,
		            L"CoopIII could not create its window. A Direct3D 11 capable display "
		            L"driver is needed.\r\n\r\nRun it with --nogui to check the game and "
		            L"start it without one.",
		            L"CoopIII Launcher", MB_ICONERROR | MB_OK);
		return 1;
	}

	FindGameAndLoadConfig(startup);

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

		// ---- the two columns ---------------------------------------------
		const float bodyTop = kTitleBar;
		const float bodyH   = screen.y - kTitleBar;
		draw->AddRectFilled(ImVec2(0, bodyTop), ImVec2(kLeftW, screen.y), theme.bgWindow);
		draw->AddRectFilled(ImVec2(kLeftW, bodyTop), ImVec2(screen.x, screen.y), theme.bgPanel);

		// The map lives behind the left column, 440 x 400 from its top, with a
		// fade so the checks never sit on texture at full strength.
		const ImVec2 mapSize(kLeftW, 400.0f);
		CityMap::Draw(draw, ImVec2(0, bodyTop), mapSize, theme, MapRoute{}, app.Seconds(),
		              app.Animates());
		CityMap::DrawFade(draw, ImVec2(0, bodyTop), mapSize, theme,
		                  MapFade{true, 0.35f, 0.92f, theme.bgWindow});

		const TitleBarResult bar = TitleBar(app, "Launcher", "v0.1.1", false);
		if (bar.minimise) app.Minimize();
		if (bar.close)    app.Close();
		if (bar.theme)    app.SetTheme(!app.IsLight());

		// ---- left: the logo, then the checks ------------------------------
		LogoWidth(ImVec2(kLeftPad, bodyTop + kLeftPad), kLogoW);

		const float listW  = kLeftW - kLeftPad * 2.0f;
		const float listH  = ListHeight(g_state.checks, listW);
		const float blockH = 20.0f + 14.0f + listH + 14.0f + 32.0f;
		const float blockY = screen.y - kLeftBottom - blockH;

		Text(ImVec2(kLeftPad, blockY), Type::SectionTitle, theme.textPrimary,
		     "Pre-launch checks");
		// All clear and the problem count cross over each other rather than
		// swapping, because the moment they swap is the moment somebody has
		// just fixed something and is looking straight at it.
		const float clear = Appear("##allclear", g_state.checks.problems == 0, 10.0f);
		if (clear > 0.0f) {
			const DrawGroup ok(draw);
			TextRight(kLeftW - kLeftPad, blockY + 1.0f, Type::BodySmall, theme.statusOk,
			          "All clear");
			ok.Fade(clear);
			ok.Move(ImVec2(0.0f, (1.0f - clear) * 6.0f * Travel()));
		}
		if (clear < 1.0f) {
			char label[32];
			std::snprintf(label, sizeof(label), "%d problem%s", g_state.checks.problems > 0 ? g_state.checks.problems : 1,
			              g_state.checks.problems == 1 ? "" : "s");
			const DrawGroup bad(draw);
			TextRight(kLeftW - kLeftPad, blockY + 1.0f, Type::FieldLabel, theme.statusFail, label);
			bad.Fade(1.0f - clear);
			bad.Move(ImVec2(0.0f, clear * -6.0f * Travel()));
		}

		DrawCheckList({ImVec2(kLeftPad, blockY + 34.0f), ImVec2(listW, listH)}, g_state.checks,
		              theme);

		// Re-checking is four file reads and an MD5; it is over before the
		// frame ends. The turn is an acknowledgement of the click, not a
		// progress bar - one revolution, then it stops.
		const float sinceCheck = app.Seconds() - g_state.checkedAt;
		const float turn = (sinceCheck < 0.5f && Ambient())
		                       ? (1.0f - EaseOut(sinceCheck / 0.5f)) * -6.2831853f
		                       : 0.0f;
		if (OutlineButton("##recheck",
		                  {ImVec2(kLeftPad, blockY + 34.0f + listH + 14.0f), ImVec2(126.0f, 32.0f)},
		                  "Check again", Icon::RotateCcw, theme, false, Type::BodySmall, 0,
		                  turn)) {
			Recheck();
			g_state.checkedAt = app.Seconds();
		}

		// ---- right: the form, or the lobby ------------------------------
		const float rightX = kLeftW + kRightPadX;
		const float rightW = screen.x - kLeftW - kRightPadX * 2.0f;
		float       y      = bodyTop + kRightPadT;

		if (g_state.lobbyView) {
			DrawLobby(theme, rightX, rightW, y, screen.y - kRightPadB);
		} else {
			Text(ImVec2(rightX, y), Type::WindowHeading, theme.textPrimary, "Join a server");
			y += 34.0f + 10.0f;
			y += TextWrapped(ImVec2(rightX, y), 470.0f, Type::Body, theme.textSecondary,
			                 "Co-op only runs when GTA III starts from here. Your name and server "
			                 "are saved to CoopIII.ini.");

			y += 32.0f;   // the form's margin-top

			// Your name, with the character counter the protocol's NICK_LEN sets.
			char counter[16];
			std::snprintf(counter, sizeof(counter), "%d / 23",
			              static_cast<int>(SanitizeNick(g_state.nick).size()));
			Field("##nick", ImVec2(rightX, y), rightW, "Your name", counter, g_state.nick,
			      sizeof(g_state.nick), theme);
			y += 72.0f + 22.0f;

			// Server address and port, 112 px for the port.
			const float portW = 112.0f;
			const float hostW = rightW - portW - 12.0f;
			Field("##host", ImVec2(rightX, y), hostW, "Server address", nullptr, g_state.host,
			      sizeof(g_state.host), theme);
			Field("##port", ImVec2(rightX + hostW + 12.0f, y), portW, "Port", nullptr, g_state.port,
			      sizeof(g_state.port), theme, 46.0f, ImGuiInputTextFlags_CharsDecimal);
			y += 72.0f + 8.0f;
			Text(ImVec2(rightX, y), Type::BodySmall, theme.textTertiary,
			     "Ask whoever runs the server. The default port is 2001.");
			y += 18.0f + 22.0f;

			// Game folder, with Browse.
			Text(ImVec2(rightX, y), Type::FieldLabel, theme.textSecondary, "Game folder");
			const float browseW = 110.0f;
			const Rect  dirBox{ImVec2(rightX, y + 26.0f), ImVec2(rightW - browseW - 8.0f, 46.0f)};
			if (TextInput("##gamedir", dirBox, g_state.gameDir, sizeof(g_state.gameDir), theme))
				Recheck();
			if (OutlineButton("##browse",
			                  {ImVec2(dirBox.Max().x + 8.0f, dirBox.pos.y), ImVec2(browseW, 46.0f)},
			                  "Browse", Icon::Folder, theme)) {
				const std::string picked =
				    App::PickFolder("Where is GTA III installed?", g_state.gameDir);
				if (!picked.empty()) {
					CopyInto(g_state.gameDir, sizeof(g_state.gameDir), picked);
					Recheck();
				}
			}

			// ---- the lobby button, pinned to the bottom -----------------------
			// Everybody waits there and the host starts everybody's game; the
			// link beside the note starts this one alone, as the button used to.
			uint16_t    port    = 0;
			const bool  ready   = g_state.checks.Ready() && FormIsValid(&port);
			const float buttonY = screen.y - kRightPadB - 18.0f - 10.0f - 60.0f;

			if (PrimaryButton("##lobby", {ImVec2(rightX, buttonY), ImVec2(rightW, 60.0f)},
			                  "Join the lobby", Icon::Users, ready, theme))
				JoinLobby(port);

			const float noteY  = buttonY + 60.0f + 10.0f;
			const char *alone  = "Start on my own";
			const float aloneW = MeasureText(Type::BodySmall, alone).x;
			const float noteW  = ready ? rightW - aloneW - 16.0f : rightW;
			if (ready && LinkText("##alone", ImVec2(rightX + rightW - aloneW, noteY), Type::BodySmall,
			                      theme.textButton, alone))
				StartGame(false);

			draw->PushClipRect(ImVec2(rightX, noteY), ImVec2(rightX + noteW, noteY + 20.0f), true);
			if (!g_state.startError.empty()) {
				Text(ImVec2(rightX, noteY), Type::BodySmall, theme.statusFail,
				     g_state.startError.c_str());
			} else if (g_state.checks.problems > 0) {
				char note[96];
				std::snprintf(note, sizeof(note), "Fix the %d problem%s on the left, then check again.",
				              g_state.checks.problems, g_state.checks.problems == 1 ? "" : "s");
				Text(ImVec2(rightX, noteY), Type::BodySmall, theme.textSecondary, note);
			} else if (!ready) {
				Text(ImVec2(rightX, noteY), Type::BodySmall, theme.textSecondary,
				     "Fill in your name, the server address and the port.");
			} else {
				char note[256];
				std::snprintf(note, sizeof(note), "Waits in %s:%u's lobby as %s.", g_state.host, port,
				              g_state.nick);
				Text(ImVec2(rightX, noteY), Type::BodySmall, theme.textTertiary, note);
			}
			draw->PopClipRect();
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		// The game is starting; there is nothing left for this window to do.
		if (g_state.launched)
			app.Close();
	}, [&] {
		if (ServiceLobby())
			app.Close();
	});

	// Out of the lobby before the window goes, so the others see this
	// launcher leave at once rather than when its connection times out.
	g_state.lobby.Leave();
	return result;
}

} // namespace coopiii::launcher
