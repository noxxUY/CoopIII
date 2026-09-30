// CoopIII Setup.
//
// Four screens, in design/screens/InstallerDowngrade, Installer,
// InstallerProgress and InstallerDone. Which one opens depends on the game:
// a copy that is already v1.0 retail skips the downgrade entirely.
//
// The work is installer-core, on a thread of its own. This file is layout and
// the one question the Setup keeps asking: what is actually in the player's
// game folder right now.
#include "installer/core.h"
#include <coopiii/version.h>

#include "launcher/core.h"

#include "ui/anim.h"
#include "ui/app.h"
#include "ui/citymap.h"
#include "ui/widgets.h"

#include <imgui_internal.h>

#include <windows.h>

#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ui;
using namespace coopiii;
using namespace coopiii::installer;

namespace coopiii::installer { void RegisterPayload(); }

namespace {

// The design's window, to the pixel.
constexpr float kWindowW  = 1120.0f;
constexpr float kWindowH  = 860.0f;
constexpr float kTitleBar = 40.0f;
constexpr float kLeftW    = 380.0f;
constexpr float kLeftPad  = 40.0f;
constexpr float kLeftBot  = 32.0f;
constexpr float kRightPadX = 44.0f;
constexpr float kRightPadT = 36.0f;
constexpr float kRightPadB = 28.0f;
constexpr float kLogoW    = 216.0f;
constexpr float kTileH    = 58.0f;

enum class Screen { Downgrade, Components, Installing, Done };

struct State {
	Screen screen = Screen::Components;

	char    gameDir[MAX_PATH] = "";
	GameExe game;               // what gta3.exe is, and whether it can be downgraded
	bool    downgrade = false;  // the player said yes on the downgrade screen
	bool    hasRecord = false;  // the Setup has installed into this folder before

	std::vector<bool> selected;
	bool              shortcut     = true;
	std::string       message;      // a failure worth showing under the buttons
	bool              messageIsError = true;

	InstallJob job;
	Progress   progress;

	// What the right column is coming from, so a screen change can be drawn
	// as one: forward slides in from the right, back from the left.
	Screen shown     = Screen::Components;
	float  direction = 1.0f;
};

State g_state;

const Manifest &Components() { return BuiltInManifest(); }

void LookAtGame() {
	g_state.game      = InspectGame(g_state.gameDir, Components());
	g_state.hasRecord = HasInstallRecord(g_state.gameDir);
	g_state.downgrade = false;
	g_state.direction = 1.0f;
	g_state.screen    = (g_state.game.present && !g_state.game.isV10) ? Screen::Downgrade
	                                                                  : Screen::Components;
}

// v1.0 already, or about to be.
bool WillBeV10() { return g_state.game.isV10 || (g_state.downgrade && g_state.game.CanDowngrade()); }

std::string ShortMd5(const std::string &md5) { return md5.size() > 8 ? md5.substr(0, 8) + "..." : md5; }

int SelectedCount() {
	int n = 0;
	for (size_t i = 0; i < g_state.selected.size(); ++i)
		if (g_state.selected[i])
			++n;
	return n;
}

std::string WithCommas(unsigned long long value) {
	std::string digits = std::to_string(value);
	std::string out;
	int         count = 0;
	for (size_t i = digits.size(); i-- > 0;) {
		out += digits[i];
		if (++count % 3 == 0 && i > 0)
			out += ',';
	}
	return std::string(out.rbegin(), out.rend());
}

// ---- the left column ------------------------------------------------------

struct Step {
	const char *name;
	std::string note;
};

void DrawLeft(App &app, const Theme &theme, ImVec2 screen) {
	ImDrawList *draw = ImGui::GetWindowDrawList();

	draw->AddRectFilled(ImVec2(0, kTitleBar), ImVec2(kLeftW, screen.y), theme.bgWindow);
	const ImVec2 mapSize(440.0f, 400.0f);
	draw->PushClipRect(ImVec2(0, kTitleBar), ImVec2(kLeftW, screen.y), true);
	CityMap::Draw(draw, ImVec2(0, kTitleBar), mapSize, theme, MapRoute{}, app.Seconds(),
	              app.Animates());
	CityMap::DrawFade(draw, ImVec2(0, kTitleBar), mapSize, theme,
	                  MapFade{true, 0.35f, 0.92f, theme.bgWindow});
	draw->PopClipRect();

	// Top: the logo, then what the Setup is for.
	const float logoH = LogoWidth(ImVec2(kLeftPad, kTitleBar + kLeftPad), kLogoW);
	TextWrapped(ImVec2(kLeftPad, kTitleBar + kLeftPad + logoH + 18.0f), 280.0f, Type::Body,
	            theme.textSecondary,
	            "Sets up the co-op mod and the Essential Pack, the mods it was built and "
	            "tested alongside.");

	// Bottom: the four steps, as a card of rows.
	const int current = g_state.screen == Screen::Downgrade    ? 0
	                    : g_state.screen == Screen::Components ? 1
	                    : g_state.screen == Screen::Installing ? 2
	                                                           : 3;

	char componentsNote[48];
	std::snprintf(componentsNote, sizeof(componentsNote), "%d of %d selected", SelectedCount(),
	              static_cast<int>(Components().components.size()));

	const Step steps[4] = {
	    {"Game version", g_state.game.isV10 ? "v1.0 retail"
	                     : g_state.downgrade ? "Downgrade planned"
	                                         : "Needs v1.0 retail"},
	    {"Components", componentsNote},
	    {"Install", "Copies files into the game folder"},
	    {"Done", "Open the launcher and play"},
	};

	const float rowH  = 12.0f + 20.0f + 2.0f + 16.0f + 12.0f;   // padding, title, gap, note
	const float cardH = rowH * 4 + 3.0f + 2.0f;
	const float cardY = screen.y - kLeftBot - cardH;
	const float titleY = cardY - 14.0f - 20.0f;

	Text(ImVec2(kLeftPad, titleY), Type::SectionTitle, theme.textPrimary, "Setup");
	const Rect card{ImVec2(kLeftPad, cardY), ImVec2(kLeftW - kLeftPad * 2.0f, cardH)};
	Card(card, theme);

	float y = cardY + 1.0f;
	for (int i = 0; i < 4; ++i) {
		const Rect row{ImVec2(card.pos.x + 1.0f, y), ImVec2(card.size.x - 2.0f, rowH)};

		// The first two steps are somewhere to go back to. Install and Done
		// are not: one is in progress and the other is over. Going back to
		// the downgrade only makes sense while there is something to
		// downgrade.
		const bool reachable = (g_state.screen == Screen::Downgrade ||
		                        g_state.screen == Screen::Components) &&
		                       i != current &&
		                       ((i == 0 && g_state.game.present && !g_state.game.isV10) || i == 1);

		Touched touch;
		if (reachable) {
			char id[24];
			std::snprintf(id, sizeof(id), "##step%d", i);
			touch = Hotspot(id, row);
			if (touch.hovered)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (touch.clicked) {
				g_state.direction = i < current ? -1.0f : 1.0f;
				g_state.screen    = i == 0 ? Screen::Downgrade : Screen::Components;
			}
			if (touch.t > 0.0f)
				draw->AddRectFilled(row.pos, row.Max(),
				                    Lift(theme.bgCard, theme, touch.t * 0.7f), 0.0f);
		}

		// Two numbers rather than two states, so a step crossing from pending
		// to current to done is a move and not a cut.
		char stepKey[24];
		std::snprintf(stepKey, sizeof(stepKey), "##stepstate%d", i);
		const ImGuiID key     = MotionKey(stepKey);
		const float   done    = Animate(key, i < current ? 1.0f : 0.0f, 12.0f);
		const float   present = Animate(key ^ 0x9E3779B9u, i == current ? 1.0f : 0.0f, 12.0f);

		const Rect badge{ImVec2(card.pos.x + 14.0f, y + (rowH - 28.0f) * 0.5f),
		                 ImVec2(28.0f, 28.0f)};
		char        n[4];
		std::snprintf(n, sizeof(n), "%d", i + 1);
		const float w = MeasureText(Type::Mono, n).x;

		if (done < 1.0f) {
			draw->AddRectFilled(badge.pos, badge.Max(), WithAlpha(theme.actionBg, present), 14.0f);
			if (present < 1.0f)
				draw->AddRect(badge.pos, badge.Max(),
				              WithAlpha(theme.borderControl, (1.0f - present) * (1.0f - done)),
				              14.0f, 0, 1.0f);
			TextMiddle(ImVec2(badge.pos.x + (28.0f - w) * 0.5f, badge.pos.y), 28.0f, Type::Mono,
			           WithAlpha(Mix(theme.textTertiary, theme.actionFg, present), 1.0f - done),
			           n);
		}
		if (done > 0.0f) {
			const DrawGroup tick(draw);
			draw->AddRectFilled(badge.pos, badge.Max(), theme.okPillBg, 14.0f);
			draw->AddRect(badge.pos, badge.Max(), theme.okPillBorder, 14.0f, 0, 1.0f);
			DrawIcon(draw, Icon::Check, ImVec2(badge.pos.x + 7.0f, badge.pos.y + 7.0f), 14.0f,
			         theme.statusOk);
			tick.Fade(done);
			tick.Scale(badge.Centre(), 0.7f + 0.3f * EaseBack(done));
		}

		const float textX = card.pos.x + 14.0f + 28.0f + 14.0f;
		Text(ImVec2(textX, y + 12.0f), Type::FieldLabel,
		     Mix(theme.textSecondary, theme.textPrimary, ImMax(done, present)), steps[i].name);
		Text(ImVec2(textX, y + 12.0f + 20.0f + 2.0f), Type::Hint,
		     Mix(theme.textTertiary, theme.textSecondary, present), steps[i].note.c_str());

		y += rowH;
		if (i < 3) {
			draw->AddLine(ImVec2(card.pos.x + 1.0f, y + 0.5f),
			              ImVec2(card.Max().x - 1.0f, y + 0.5f), theme.border, 1.0f);
			y += 1.0f;
		}
	}
}

// ---- shared pieces of the right column ------------------------------------

float DrawHeading(const Theme &theme, float x, float y, float width, const char *title,
                  const char *body) {
	Text(ImVec2(x, y), Type::WindowHeading, theme.textPrimary, title);
	return 34.0f + 6.0f +
	       TextWrapped(ImVec2(x, y + 34.0f + 6.0f), ImMin(width, 560.0f), Type::Body,
	                   theme.textSecondary, body);
}

// The game folder field, its Browse button and the one-line verdict under it.
float DrawGameFolder(const Theme &theme, float x, float y, float width) {
	Text(ImVec2(x, y), Type::FieldLabel, theme.textSecondary, "Game folder");

	const float browseW = 110.0f;
	const Rect  box{ImVec2(x, y + 18.0f + 8.0f), ImVec2(width - browseW - 8.0f, 44.0f)};
	if (TextInput("##gamedir", box, g_state.gameDir, sizeof(g_state.gameDir), theme))
		LookAtGame();
	if (OutlineButton("##browse", {ImVec2(box.Max().x + 8.0f, box.pos.y), ImVec2(browseW, 44.0f)},
	                  "Browse", Icon::Folder, theme)) {
		const std::string picked = App::PickFolder("Where is GTA III installed?", g_state.gameDir);
		if (!picked.empty()) {
			std::snprintf(g_state.gameDir, sizeof(g_state.gameDir), "%s", picked.c_str());
			LookAtGame();
		}
	}

	const float statusY = box.Max().y + 8.0f;
	const GameExe &g = g_state.game;
	std::string verdict;
	Icon        icon;
	ImU32       colour;
	if (!g.present) {
		verdict = "no gta3.exe in this folder";
		icon    = Icon::CircleX;
		colour  = theme.statusFail;
	} else if (g.running) {
		verdict = "GTA III is running. Close it before installing.";
		icon    = Icon::CircleX;
		colour  = theme.statusFail;
	} else if (g.isV10) {
		verdict = "gta3.exe is v1.0 retail";
		icon    = Icon::CircleCheck;
		colour  = theme.statusOk;
	} else {
		verdict = g.buildName.empty()
		              ? "gta3.exe is a build the Setup does not recognise (MD5 " + ShortMd5(g.md5) + ")"
		              : "gta3.exe is the " + g.buildName + " build";
		icon   = Icon::TriangleAlert;
		colour = theme.statusWarn;
	}
	DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(x, statusY + 1.0f), 16.0f, colour);
	Text(ImVec2(x + 24.0f, statusY), Type::BodySmall, theme.textSecondary, verdict.c_str());

	return statusY + 18.0f - y;
}

// ---- the downgrade screen -------------------------------------------------

void DrawDowngrade(App &app, const Theme &theme, ImVec2 screen, float x, float width) {
	ImDrawList   *draw = ImGui::GetWindowDrawList();
	const GameExe &g   = g_state.game;
	float         y    = kTitleBar + kRightPadT;

	// Two cases. A build the Setup has a patch for is downgraded as the
	// install's first step. Anything else - the Steam exe above all, which is
	// wrapped in SteamStub so that a patch from it would have to carry most of
	// v1.0 as it is - the Setup does not touch: the player downgrades it with
	// a community tool and comes back. docs/installer.md section 2.
	const bool         patchable = g.CanDowngrade();
	const Downgrader  &tool      = Components().downgrader;
	const std::string  howTo     = tool.url.empty()
	                                   ? "Downgrade your own copy with a community guide"
	                                   : "Downgrade your own copy following the guide";
	const std::string  intro =
	    patchable ? std::string("CoopIII's addresses only match GTA III v1.0 retail. This copy is "
	                            "another build, so the Setup patches it back to v1.0 as the first "
	                            "step of the install.")
	    : (g.buildName.empty() ? std::string("This gta3.exe is not a build the Setup knows.")
	                           : "Your GTA III is the " + g.buildName + " version.") +
	          " CoopIII needs v1.0. " + howTo +
	          ", keep a backup of the original gta3.exe, then press Check again.";

	y += DrawHeading(theme, x, y, width, patchable ? "Downgrade to v1.0" : "Downgrade GTA III first",
	                 intro.c_str());
	y += 22.0f;
	y += DrawGameFolder(theme, x, y, width);
	y += 22.0f;

	Text(ImVec2(x, y), Type::SectionTitle, theme.textPrimary, "What changes");
	y += 20.0f + 10.0f;

	// The table: what the exe is now, and what it will be.
	const float headerH = 10.0f + 16.0f + 10.0f;
	const float rowH    = 13.0f + 20.0f + 13.0f;
	const Rect  table{ImVec2(x, y), ImVec2(width, headerH + rowH * 3 + 3.0f + 2.0f)};
	Card(table, theme);
	draw->AddRectFilled(ImVec2(table.pos.x + 1.0f, table.pos.y + 1.0f),
	                    ImVec2(table.Max().x - 1.0f, table.pos.y + headerH), theme.bgWindow,
	                    radius::kCard, ImDrawFlags_RoundCornersTop);

	const float colA = table.pos.x + 16.0f;
	const float colB = colA + 96.0f + 16.0f;
	const float colC = colB + (width - 96.0f - 48.0f) * 0.4f + 16.0f;

	draw->AddCircleFilled(ImVec2(colB + 3.5f, table.pos.y + headerH * 0.5f), 3.5f,
	                      theme.statusWarn, 12);
	Text(ImVec2(colB + 15.0f, table.pos.y + 10.0f), Type::MetaLabel, theme.statusWarn, "Now");
	draw->AddCircleFilled(ImVec2(colC + 3.5f, table.pos.y + headerH * 0.5f), 3.5f, theme.statusOk,
	                      12);
	Text(ImVec2(colC + 15.0f, table.pos.y + 10.0f), Type::MetaLabel, theme.statusOk,
	     "After the downgrade");

	char sizeNow[32], expectedSize[32];
	std::snprintf(sizeNow, sizeof(sizeNow), "%s bytes", WithCommas(g.size).c_str());
	std::snprintf(expectedSize, sizeof(expectedSize), "%s bytes",
	              WithCommas(launcher::GAME_SIZE_BYTES).c_str());

	struct Row {
		const char *label;
		std::string now;
		const char *after;
		bool        mono;
	};
	const Row rows[3] = {
	    {"Build", g.buildName.empty() ? "Not recognised" : g.buildName, "v1.0 retail", false},
	    {"Size", g.present ? sizeNow : "-", expectedSize, false},
	    {"MD5", g.present ? ShortMd5(g.md5) : "-", launcher::GAME_MD5, true},
	};

	float rowY = table.pos.y + headerH + 1.0f;
	for (int i = 0; i < 3; ++i) {
		Text(ImVec2(colA, rowY + 14.0f), Type::BodySmall, theme.textTertiary, rows[i].label);
		Text(ImVec2(colB, rowY + 13.0f), Type::Body, theme.textSecondary, rows[i].now.c_str());
		if (rows[i].mono)
			Text(ImVec2(colC, rowY + 13.0f), Type::Hint, theme.textPrimary, rows[i].after);
		else
			Text(ImVec2(colC, rowY + 13.0f), Type::FieldLabel, theme.textPrimary, rows[i].after);
		rowY += rowH;
		if (i < 2) {
			draw->AddLine(ImVec2(table.pos.x + 1.0f, rowY + 0.5f),
			              ImVec2(table.Max().x - 1.0f, rowY + 0.5f), theme.border, 1.0f);
			rowY += 1.0f;
		}
	}
	y = table.Max().y + 22.0f;

	if (patchable) {
		const float keptW =
		    Text(ImVec2(x, y), Type::Body, theme.textSecondary, "The original is kept as ");
		const float bakW =
		    Text(ImVec2(x + keptW, y + 1.0f), Type::Mono, theme.textPrimary, "gta3.exe.bak");
		Text(ImVec2(x + keptW + bakW, y), Type::Body, theme.textSecondary,
		     ", and Uninstall puts it back.");
		y += 18.0f + 20.0f;
	}

	// Where the patch comes from and the Steam caveat, or why the Setup will
	// not do it itself.
	std::string note;
	if (!g.localPatch.empty())
		note = "Using the patch beside this Setup. Steam can put the newer exe back after an update "
		       "or a file check; if the launcher says the version is wrong again, run the Setup once "
		       "more.";
	else if (g.knownPatch)
		note = "The patch for this build is downloaded from CoopIII's release and checked before it "
		       "is applied. Steam can put the newer exe back after an update or a file check; if the "
		       "launcher says the version is wrong again, run the Setup once more.";
	else if (!g.buildName.empty())
		note = "The Setup leaves this gta3.exe exactly as it is. The " + g.buildName +
		       " exe is wrapped in the store's copy protection, so it cannot be turned into v1.0 by "
		       "a small patch, and CoopIII does not try. Steam can put the newer exe back after an "
		       "update or a file check; if the launcher says the version is wrong again, downgrade "
		       "again and press Check again.";
	else
		note = "The Setup leaves this gta3.exe exactly as it is. Its MD5 is " + g.md5 +
		       ", which is neither v1.0 nor a build CoopIII knows. Once it is v1.0, press Check "
		       "again.";
	const float noteH = MeasureWrapped(Type::BodySmall, width - 60.0f, note.c_str()).y;
	const Rect  noteBox{ImVec2(x, y), ImVec2(width, noteH + 28.0f)};
	Card(noteBox, theme, radius::kInput, theme.bgWindow, theme.border);
	DrawIcon(draw, patchable ? Icon::Info : Icon::TriangleAlert, ImVec2(x + 16.0f, y + 15.0f), 18.0f,
	         patchable ? theme.textTertiary : theme.statusWarn);
	TextWrapped(ImVec2(x + 16.0f + 18.0f + 12.0f, y + 14.0f), width - 62.0f, Type::BodySmall,
	            theme.textSecondary, note.c_str());
	y = noteBox.Max().y + 14.0f;

	// The background reading, as a plain link under the note.
	if (!patchable && !tool.infoUrl.empty() &&
	    LinkText("##moreinfo", ImVec2(x, y), Type::BodySmall, theme.textSecondary,
	             (tool.infoName.empty() ? std::string("More info") : tool.infoName).c_str()))
		ShellExecuteA(nullptr, "open", tool.infoUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

	// Footer.
	const float footerY = screen.y - kRightPadB - 56.0f;
	if (!g_state.message.empty())
		TextWrapped(ImVec2(x, footerY - 24.0f), width, Type::BodySmall,
		            g_state.messageIsError ? theme.statusFail : theme.textTertiary,
		            g_state.message.c_str());

	const std::string guide = tool.name.empty() ? std::string("Open the guide") : tool.name;
	const float       mainW =
	    patchable ? 220.0f
	              : ImMin(width - 100.0f - 150.0f - 20.0f,
	                      MeasureText(Type::ButtonSmall, guide.c_str()).x + 18.0f + 10.0f + 52.0f);
	float       left  = x + width - mainW - 10.0f;

	if (!patchable) {
		// Re-reads gta3.exe: the player has just run the downgrader, or has
		// not yet, and either way the screen should say which.
		const float checkW = 150.0f;
		left -= checkW;
		if (OutlineButton("##checkagain", {ImVec2(left, footerY), ImVec2(checkW, 56.0f)},
		                  "Check again", Icon::RotateCcw, theme, false, Type::ButtonSmall)) {
			LookAtGame();
			g_state.messageIsError = false;
			g_state.message =
			    g_state.game.isV10 ? std::string()
			    : !g_state.game.present
			        ? "There is no gta3.exe in that folder."
			        : "gta3.exe is still not v1.0 (MD5 " + ShortMd5(g_state.game.md5) + ").";
		}
		left -= 10.0f;
	}

	if (OutlineButton("##cancel", {ImVec2(left - 100.0f, footerY), ImVec2(100.0f, 56.0f)}, "Cancel",
	                  Icon::None, theme, false, Type::ButtonSmall))
		app.Close();

	if (!patchable) {
		// The guide the manifest names: a page, never a file (Parse refuses one).
		// With none set, the button stays off rather than pointing at a guess.
		if (PrimaryButton("##downgrader", {ImVec2(x + width - mainW, footerY), ImVec2(mainW, 56.0f)},
		                  guide.c_str(), Icon::ArrowUpRight, !tool.url.empty(), theme,
		                  Type::ButtonSmall))
			ShellExecuteA(nullptr, "open", tool.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}

	// Nothing is patched here: the downgrade is the install's first step, so it
	// shows in the same progress as everything else and a failure stops the
	// rest rather than leaving mods on an exe CoopIII cannot use.
	if (PrimaryButton("##downgrade", {ImVec2(x + width - mainW, footerY), ImVec2(mainW, 56.0f)},
	                  "Continue", Icon::RotateCcw, !g.running, theme)) {
		g_state.downgrade = true;
		g_state.message.clear();
		g_state.direction = 1.0f;
		g_state.screen    = Screen::Components;
	}
}

// ---- the components screen ------------------------------------------------

void DrawComponentTile(const Theme &theme, Rect box, size_t index, const Component &c) {
	ImDrawList *draw    = ImGui::GetWindowDrawList();
	const bool  locked  = c.required;
	const bool  ready   = c.Ready();

	char id[32];
	std::snprintf(id, sizeof(id), "##tile%zu", index);

	Touched touch;
	if (!locked) {
		touch = Hotspot(id, box);
		if (touch.clicked)
			g_state.selected[index] = !g_state.selected[index];
		if (touch.hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	} else {
		touch.key = ImGui::GetID(id);
	}
	// Who made it and under what terms, for every tile: most of the Essential
	// Pack is other people's work, and this is where they get the credit.
	if (ImGui::IsMouseHoveringRect(box.pos, box.Max()) && !c.homepage.empty())
		ImGui::SetTooltip("%s\nLicence: %s\n%s", c.name.c_str(),
		                  c.license.empty() ? "not stated" : c.license.c_str(), c.homepage.c_str());

	Card(box, theme, radius::kPrimary, Lift(theme.bgCard, theme, touch.t * 0.6f),
	     locked ? theme.borderStrong : Mix(theme.border, theme.borderStrong, touch.t * 2.0f));

	// The tick, the same as the one in Checkbox: it fills from the middle and
	// lands just past full size.
	const Rect  mark{ImVec2(box.pos.x + 14.0f, box.pos.y + (kTileH - 18.0f) * 0.5f),
	                 ImVec2(18.0f, 18.0f)};
	const float on = Animate(touch.Also(1), g_state.selected[index] ? 1.0f : 0.0f, 20.0f);
	if (on < 1.0f)
		draw->AddRect(mark.pos, mark.Max(), WithAlpha(theme.borderControl, 1.0f - on), 4.0f, 0,
		              1.5f);
	if (on > 0.0f) {
		const DrawGroup ticked(draw);
		draw->AddRectFilled(mark.pos, mark.Max(), theme.actionBg, 4.0f);
		DrawIcon(draw, Icon::Check, ImVec2(mark.pos.x + 2.0f, mark.pos.y + 2.0f), 14.0f,
		         theme.actionFg, 2.6f);
		ticked.Fade(on);
		ticked.Scale(mark.Centre(), 0.55f + 0.45f * EaseBack(on));
	}

	const float textX = box.pos.x + 14.0f + 18.0f + 12.0f;
	float       right = box.Max().x - 14.0f;

	if (!c.version.empty()) {
		const std::string version =
		    std::isdigit(static_cast<unsigned char>(c.version[0])) ? "v" + c.version : c.version;
		right -= TextRight(right, box.pos.y + 20.0f, Type::MonoSmall, theme.textTertiary,
		                   version.c_str()) +
		         10.0f;
	} else if (!ready) {
		right -= TextRight(right, box.pos.y + 20.0f, Type::Hint, theme.statusWarn,
		                   "no download yet") +
		         10.0f;
	}

	const float nameW = Text(ImVec2(textX, box.pos.y + 10.0f), Type::FieldLabel,
	                         theme.textPrimary, c.name.c_str());
	// Only if there is room left of the right-hand note: on a narrow tile the
	// chip and the note otherwise print over each other.
	const float chipW = MeasureText(Type::Hint, "Required").x + 18.0f;
	if (locked && textX + nameW + 8.0f + chipW <= right - 8.0f)
		Chip(ImVec2(textX + nameW + 8.0f, box.pos.y + 11.0f), "Required", theme);

	Text(ImVec2(textX, box.pos.y + 10.0f + 20.0f + 1.0f), Type::Hint, theme.textTertiary,
	     c.description.c_str());
}

void DrawComponents(App &app, const Theme &theme, ImVec2 screen, float x, float width) {
	float y = kTitleBar + kRightPadT;

	y += DrawHeading(theme, x, y, width, "Choose what to install",
	                 g_state.downgrade && !g_state.game.isV10
	                     ? "gta3.exe is downgraded first, then CoopIII goes in with the Essential "
	                       "Pack. Untick anything you already manage yourself."
	                     : "CoopIII goes in with the Essential Pack by default. Untick anything you "
	                       "already manage yourself.");
	y += 22.0f;
	y += DrawGameFolder(theme, x, y, width);
	y += 22.0f;

	const auto &all = Components().components;
	if (all.empty()) {
		std::string why;
		BuiltInManifest(&why);
		TextWrapped(ImVec2(x, y), width, Type::Body, theme.statusFail,
		            ("This Setup's list of components is damaged (" + why +
		             "). Download the Setup again.")
		                .c_str());
		return;
	}
	Text(ImVec2(x, y), Type::SectionTitle, theme.textPrimary, "Components");

	char count[48];
	std::snprintf(count, sizeof(count), "%d of %d selected", SelectedCount(),
	              static_cast<int>(all.size()));
	const float buttonW = 96.0f;
	const bool  anyOn   = SelectedCount() > 1;
	if (OutlineButton("##selectall",
	                  {ImVec2(x + width - buttonW, y - 4.0f), ImVec2(buttonW, 28.0f)},
	                  anyOn ? "Select none" : "Select all", Icon::None, theme, false,
	                  Type::Hint)) {
		// "Select all" means all of what can actually be installed. A
		// component without a download stays exactly as it was rather than
		// being ticked into a failure the player did not ask for.
		for (size_t i = 0; i < all.size(); ++i)
			if (all[i].required)
				g_state.selected[i] = true;
			else if (all[i].Ready())
				g_state.selected[i] = !anyOn;
	}
	TextRight(x + width - buttonW - 14.0f, y + 1.0f, Type::BodySmall, theme.textTertiary, count);

	y += 20.0f + 10.0f;

	// The first component spans both columns; the rest are a two-column grid.
	const float gap   = 8.0f;
	const float halfW = (width - gap) * 0.5f;
	DrawComponentTile(theme, {ImVec2(x, y), ImVec2(width, kTileH)}, 0, all[0]);
	y += kTileH + gap;

	for (size_t i = 1; i < all.size(); ++i) {
		const size_t at    = i - 1;
		const float  tileX = (at % 2 == 0) ? x : x + halfW + gap;
		const float  tileY = y + static_cast<float>(at / 2) * (kTileH + gap);
		DrawComponentTile(theme, {ImVec2(tileX, tileY), ImVec2(halfW, kTileH)}, i, all[i]);
	}

	// Footer.
	const float footerY = screen.y - kRightPadB - 56.0f;
	if (!g_state.message.empty())
		TextWrapped(ImVec2(x, footerY - 24.0f), width, Type::BodySmall,
		            g_state.messageIsError ? theme.statusFail : theme.textTertiary,
		            g_state.message.c_str());

	// Everything the Setup wrote here is in CoopIII-Setup.record, so it can
	// all come out again: files removed, replaced ones and gta3.exe put back.
	if (g_state.hasRecord &&
	    OutlineButton("##uninstall", {ImVec2(x, footerY), ImVec2(130.0f, 56.0f)}, "Uninstall",
	                  Icon::RotateCcw, theme, false, Type::ButtonSmall)) {
		std::vector<std::string> log;
		std::string              error;
		const bool               ok = Uninstall(g_state.gameDir, &log, &error);
		LookAtGame();
		g_state.messageIsError = !ok;
		g_state.message        = ok ? "Uninstalled. Everything the Setup put in this folder has been "
		                              "taken out, and anything it replaced is back."
		                            : error;
		if (!ok && !log.empty())
			g_state.message += " (" + log.back() + ")";
	}

	const float installW = 200.0f;
	if (OutlineButton("##cancel",
	                  {ImVec2(x + width - installW - 10.0f - 100.0f, footerY),
	                   ImVec2(100.0f, 56.0f)},
	                  "Cancel", Icon::None, theme, false, Type::ButtonSmall))
		app.Close();

	const bool canInstall = WillBeV10() && !g_state.game.running && SelectedCount() > 0;
	if (PrimaryButton("##install", {ImVec2(x + width - installW, footerY), ImVec2(installW, 56.0f)},
	                  "Install", Icon::Download, canInstall, theme)) {
		InstallOptions options;
		options.gameDir   = g_state.gameDir;
		options.downgrade = !g_state.game.isV10;
		for (size_t i = 0; i < all.size(); ++i)
			if (g_state.selected[i])
				options.components.push_back(all[i].id);
		g_state.message.clear();
		g_state.job.Start(options);
		g_state.direction = 1.0f;
		g_state.screen    = Screen::Installing;
	}
}

// ---- the progress screen --------------------------------------------------

void DrawInstalling(App &app, const Theme &theme, ImVec2 screen, float x, float width) {
	ImDrawList *draw = ImGui::GetWindowDrawList();
	g_state.progress = g_state.job.Snapshot();
	const Progress &p = g_state.progress;

	float y = kTitleBar + kRightPadT;
	y += DrawHeading(theme, x, y, width, "Installing",
	                 "Keep this window open until it finishes. GTA III should stay closed "
	                 "meanwhile.");
	y += 24.0f;

	char headline[64];
	std::snprintf(headline, sizeof(headline), "%d of %d installed", p.finished, p.total);
	Text(ImVec2(x, y), Type::FieldLabel, theme.textPrimary, headline);
	const float fraction = p.total > 0 ? static_cast<float>(p.finished) / p.total : 0.0f;
	char        percent[16];
	std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
	TextRight(x + width, y, Type::FieldLabel, theme.textPrimary, percent);
	y += 18.0f + 8.0f;
	ProgressBar({ImVec2(x, y), ImVec2(width, 8.0f)}, fraction, theme, "##overall");
	y += 8.0f + 24.0f;

	// One tile per component, in the same shape the choosing screen used.
	const float gap   = 8.0f;
	const float halfW = (width - gap) * 0.5f;

	auto tile = [&](Rect box, const StepProgress &step) {
		Card(box, theme, radius::kPrimary, theme.bgCard,
		     step.state == StepState::Working ? theme.borderStrong : theme.border);

		ImU32 colour = theme.textTertiary;
		Icon  icon   = Icon::Info;
		switch (step.state) {
		case StepState::Done:    colour = theme.statusOk;   icon = Icon::CircleCheck; break;
		case StepState::Failed:  colour = theme.statusFail; icon = Icon::CircleX;     break;
		case StepState::Working: colour = theme.textPrimary; icon = Icon::RotateCcw;  break;
		default: break;
		}
		// The one honest spinner in the Setup: a component that is working is
		// downloading or copying, and that really is taking as long as it is
		// turning for.
		const float turn =
		    (step.state == StepState::Working && Ambient())
		        ? -6.2831853f * (static_cast<float>(std::fmod(ImGui::GetTime(), 1.1)) / 1.1f)
		        : 0.0f;
		DrawIcon(draw, icon, ImVec2(box.pos.x + 14.0f, box.pos.y + (box.size.y - 18.0f) * 0.5f),
		         18.0f, colour, 2.0f, turn);
		TextMiddle(ImVec2(box.pos.x + 14.0f + 18.0f + 12.0f, box.pos.y), box.size.y,
		           Type::FieldLabel,
		           step.state == StepState::Waiting ? theme.textTertiary : theme.textPrimary,
		           step.name.c_str());

		const char *note = step.state == StepState::Done      ? "Done"
		                   : step.state == StepState::Working
		                       ? (step.note.find("MB") != std::string::npos ? step.note.c_str()
		                                                                    : "Installing")
		                   : step.state == StepState::Failed  ? "Failed"
		                   : step.state == StepState::Skipped ? "Skipped"
		                                                      : "Waiting";
		TextRight(box.Max().x - 14.0f, box.pos.y + (box.size.y - 18.0f) * 0.5f, Type::BodySmall,
		          step.state == StepState::Done   ? theme.statusOk
		          : step.state == StepState::Failed ? theme.statusFail
		          : step.state == StepState::Working ? theme.textPrimary
		                                             : theme.textTertiary,
		          note);
		if ((step.state == StepState::Failed || step.state == StepState::Skipped) &&
		    !step.note.empty() && ImGui::IsMouseHoveringRect(box.pos, box.Max()))
			ImGui::SetTooltip("%s", step.note.c_str());
	};

	// As many rows as there are steps, shrunk if need be so the last row
	// stays clear of the details console under it.
	const float rows      = 1.0f + std::ceil((static_cast<float>(p.steps.size()) - 1.0f) * 0.5f);
	const float detailTop = screen.y - kRightPadB - 56.0f - 18.0f - 130.0f - 26.0f - 12.0f;
	const float tileH     = ImClamp((detailTop - y) / ImMax(rows, 1.0f) - gap, 36.0f, 52.0f);
	if (!p.steps.empty()) {
		tile({ImVec2(x, y), ImVec2(width, tileH)}, p.steps[0]);
		y += tileH + gap;
		for (size_t i = 1; i < p.steps.size(); ++i) {
			const size_t at = i - 1;
			tile({ImVec2((at % 2 == 0) ? x : x + halfW + gap,
			             y + static_cast<float>(at / 2) * (tileH + gap)),
			      ImVec2(halfW, tileH)},
			     p.steps[i]);
		}
		y += static_cast<float>((p.steps.size() + 0) / 2) * (tileH + gap);
	}

	// Details, in the console's own shape.
	const float footerY  = screen.y - kRightPadB - 56.0f;
	const float detailsY = footerY - 18.0f - 130.0f;
	Text(ImVec2(x, detailsY - 26.0f), Type::FieldLabel, theme.textSecondary, "Details");

	const Rect box{ImVec2(x, detailsY), ImVec2(width, 130.0f)};
	Card(box, theme, radius::kInput, theme.bgConsole);

	std::vector<ConsoleLine> lines;
	lines.reserve(p.details.size());
	for (const std::string &detail : p.details) {
		ConsoleLine line;
		// The detail already carries its "[ ok ]" prefix; split it back off so
		// it lands in the console's tag column.
		const size_t close = detail.find(']');
		line.tag  = close == std::string::npos ? "" : detail.substr(0, close + 1);
		line.text = close == std::string::npos ? detail : detail.substr(close + 2);
		line.tagColour = line.tag.find("ok") != std::string::npos    ? theme.statusOk
		                 : line.tag.find("!!") != std::string::npos ? theme.statusFail
		                                                            : theme.textTertiary;
		line.textColour = theme.textSecondary;
		lines.push_back(std::move(line));
	}
	Console(box, lines, theme, true, "##details");

	if (p.running) {
		if (OutlineButton("##cancel", {ImVec2(x + width - 110.0f, footerY), ImVec2(110.0f, 56.0f)},
		                  "Cancel", Icon::None, theme, false, Type::ButtonSmall))
			g_state.job.Cancel();
	} else {
		g_state.direction = 1.0f;
		g_state.screen    = Screen::Done;
		// What the folder holds now, once, rather than an MD5 of gta3.exe every frame.
		g_state.game      = InspectGame(g_state.gameDir, Components());
		g_state.hasRecord = HasInstallRecord(g_state.gameDir);
	}
	(void)app;
}

// ---- the done screen ------------------------------------------------------

void DrawDone(App &app, const Theme &theme, ImVec2 screen, float x, float width) {
	ImDrawList *draw = ImGui::GetWindowDrawList();
	float       y    = kTitleBar + kRightPadT;

	const Progress &p = g_state.progress;

	// The green tick.
	const ImVec2 centre(x + 27.0f, y + 27.0f);
	draw->AddCircleFilled(centre, 27.0f, theme.okPillBg, 40);
	draw->AddCircle(centre, 27.0f, theme.okPillBorder, 40, 1.0f);
	DrawIcon(draw, Icon::Check, ImVec2(centre.x - 13.0f, centre.y - 13.0f), 26.0f, theme.statusOk);
	y += 54.0f + 14.0f;

	const bool allDone = !p.failed && p.finished == p.total;
	y += DrawHeading(theme, x, y, width,
	                 allDone ? "Ready to play co-op" : "Finished, with problems",
	                 allDone ? "CoopIII and the Essential Pack are installed. Start GTA III from "
	                           "the launcher whenever you want to play together. Started any "
	                           "other way, it stays single player."
	                         : "Some components did not install. The details on the previous "
	                           "screen say which, and why. CoopIII itself is what matters: if "
	                           "that one went in, the launcher will start.");
	y += 24.0f;

	// The summary.
	struct Row {
		const char *label;
		std::string value;
		bool        mono;
	};
	char installed[64];
	std::snprintf(installed, sizeof(installed), "%d of %d done", p.finished, p.total);

	const bool  v10     = g_state.game.isV10;   // looked at once, when the install ended
	const Row rows[4] = {
	    {"Game folder", g_state.gameDir, true},
	    {"Game version",
	     !v10              ? "Not v1.0 - the downgrade did not finish"
	     : p.downgraded    ? "v1.0 retail, original kept as " + p.backup
	                       : std::string("v1.0 retail"),
	     false},
	    {"Installed", installed, false},
	    {"Launcher", "coopiii-launcher.exe", true},
	};

	const float rowH = 49.0f;
	const Rect  table{ImVec2(x, y), ImVec2(width, rowH * 4 + 3.0f + 2.0f)};
	Card(table, theme);
	float rowY = table.pos.y + 1.0f;
	for (int i = 0; i < 4; ++i) {
		Text(ImVec2(table.pos.x + 16.0f, rowY + (rowH - 20.0f) * 0.5f), Type::Body,
		     theme.textSecondary, rows[i].label);
		TextRight(table.Max().x - 16.0f, rowY + (rowH - 20.0f) * 0.5f,
		          rows[i].mono ? Type::Mono : Type::FieldLabel, theme.textPrimary,
		          rows[i].value.c_str());
		rowY += rowH;
		if (i < 3) {
			draw->AddLine(ImVec2(table.pos.x + 1.0f, rowY + 0.5f),
			              ImVec2(table.Max().x - 1.0f, rowY + 0.5f), theme.border, 1.0f);
			rowY += 1.0f;
		}
	}
	y = table.Max().y + 22.0f;

	Checkbox("##shortcut", ImVec2(x, y), &g_state.shortcut,
	         "Put a launcher shortcut on the desktop", theme);

	// Footer.
	const float footerY = screen.y - kRightPadB - 56.0f;
	if (!g_state.message.empty())
		TextWrapped(ImVec2(x, footerY - 24.0f), width, Type::BodySmall, theme.textTertiary,
		            g_state.message.c_str());

	const float openW = 220.0f;
	if (OutlineButton("##close",
	                  {ImVec2(x + width - openW - 10.0f - 100.0f, footerY), ImVec2(100.0f, 56.0f)},
	                  "Close", Icon::None, theme, false, Type::ButtonSmall))
		app.Close();

	const std::string launcherPath =
	    launcher::Join(g_state.gameDir, "coopiii-launcher.exe");
	const bool haveLauncher = launcher::FileExists(launcherPath);

	if (PrimaryButton("##open", {ImVec2(x + width - openW, footerY), ImVec2(openW, 56.0f)},
	                  "Open launcher", Icon::Play, haveLauncher, theme)) {
		if (g_state.shortcut &&
		    !CreateDesktopShortcut(launcherPath, "CoopIII", g_state.gameDir))
			g_state.message = "The shortcut could not be created, but everything else is in.";
		ShellExecuteA(nullptr, "open", launcherPath.c_str(), nullptr, g_state.gameDir,
		              SW_SHOWNORMAL);
		app.Close();
	}
}

} // namespace

int main() {
	AppOptions options;
	options.title          = "CoopIII Setup";
	options.width          = static_cast<int>(kWindowW);
	options.height         = static_cast<int>(kWindowH);
	options.resizable      = false;
	options.maximizeBox    = false;
	options.titleBarHeight = kTitleBar;

	App app(options);
	if (!app.Ok()) {
		MessageBoxW(nullptr,
		            L"CoopIII Setup could not create its window. A Direct3D 11 capable display "
		            L"driver is needed.",
		            L"CoopIII Setup", MB_ICONERROR | MB_OK);
		return 1;
	}

	// The files this Setup carries inside itself: CoopIII and its defaults.
	RegisterPayload();

	std::snprintf(g_state.gameDir, sizeof(g_state.gameDir), "%s",
	              launcher::FindGameDir().c_str());
	// Required components stay selected regardless - the player cannot
	// untick them either. An optional one starts the way the manifest says,
	// and never ticked while it has no download to install from.
	const auto &components = Components().components;
	g_state.selected.resize(components.size());
	for (size_t i = 0; i < components.size(); ++i)
		g_state.selected[i] =
		    components[i].required || (components[i].selected && components[i].Ready());
	LookAtGame();

	return app.Run([&] {
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

		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(kLeftW, kTitleBar), screen,
		                                          theme.bgPanel);
		DrawLeft(app, theme, screen);

		const TitleBarResult bar = TitleBar(app, "Installer", "v" COOPIII_VERSION, false);
		if (bar.minimise) app.Minimize();
		if (bar.close)    app.Close();
		if (bar.theme)    app.SetTheme(!app.IsLight());

		const float x     = kLeftW + kRightPadX;
		const float width = screen.x - kLeftW - kRightPadX * 2.0f;

		// A screen change is one move: the new column comes in from the side
		// it logically lies on. The clock is restarted here rather than
		// wherever the screen was set, so every route into a screen - a
		// button, the stepper, a folder that turned out to be v1.0 after all
		// - arrives the same way.
		if (g_state.screen != g_state.shown) {
			g_state.shown = g_state.screen;
			Restart("##screen");
		}
		const float arrived = EaseOut(Since("##screen") / 0.28f);

		// The details console on the installing screen is an ImGui child, and
		// a child draws into a list of its own - so it is the one thing that
		// does not slide in with the rest. It is empty at that moment anyway.
		const DrawGroup page(ImGui::GetWindowDrawList());
		switch (g_state.screen) {
		case Screen::Downgrade:  DrawDowngrade(app, theme, screen, x, width); break;
		case Screen::Components: DrawComponents(app, theme, screen, x, width); break;
		case Screen::Installing: DrawInstalling(app, theme, screen, x, width); break;
		case Screen::Done:       DrawDone(app, theme, screen, x, width); break;
		}
		if (arrived < 1.0f) {
			page.Fade(arrived);
			page.Move(ImVec2((1.0f - arrived) * 28.0f * g_state.direction * Travel(), 0.0f));
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	});
}
