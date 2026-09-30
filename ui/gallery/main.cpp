// ui-gallery: every component in design/DESIGN.md §6, in both themes.
//
// Not shipped. It exists so a widget can be looked at on its own - and so a
// change to the theme or the city map can be checked without starting the
// launcher, the server and the installer one after another.
#include "ui/anim.h"
#include "ui/app.h"
#include "ui/citymap.h"
#include "ui/widgets.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ui;

namespace {

char g_name[64]   = "noxx";
char g_host[128]  = "192.168.1.40";
char g_port[8]    = "2001";
bool g_friendly   = false;
bool g_missionOff = true;
bool g_checked    = true;
int  g_wanted     = 0;
int  g_filter     = 0;
float g_progress  = 0.62f;
bool  g_dialog      = false;
bool  g_forceMotion = false;
bool  g_fastMap     = false;
bool  g_live        = true;    // whether the primary button is alive
int   g_step        = 2;       // where the stepper is
int   g_said        = 0;       // how many lines have been added to the console

const char *const kWanted[] = {"Per player", "Shared", "Off"};
const char *const kFilters[] = {"All", "Players", "Chat"};
const char *const kSteps[]   = {"Game version", "Components", "Install", "Done"};

// The console's own lines, so a click can add one and the arrival can be
// watched rather than described.
const char *const kSayings[] = {
    "marta joined (slot 1, net 2)",
    "marta: meet at the docks",
    "peer 2 round trip 38 ms",
    "rejected peer 3 (reason 1: bad version)",
    "noxx left (slot 0)",
};

void Section(ImVec2 pos, const char *title, const Theme &theme) {
	Text(pos, Type::SectionTitle, theme.textPrimary, title);
}

} // namespace

int main() {
	AppOptions options;
	options.title       = "CoopIII UI gallery";
	options.width       = 1180;
	options.height      = 860;
	options.resizable   = true;
	options.maximizeBox = true;

	App app(options);
	if (!app.Ok()) {
		MessageBoxW(nullptr, L"Could not create the window or the Direct3D device.",
		            L"CoopIII UI gallery", MB_ICONERROR);
		return 1;
	}

	return app.Run([&] {
		const Theme &theme = app.CurrentTheme();
		const ImVec2 screen = ImGui::GetIO().DisplaySize;

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(screen);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme.bgWindow));
		ImGui::Begin("##root", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		                 ImGuiWindowFlags_NoSavedSettings);

		ForceAmbient(app.Animates() || g_forceMotion);

		const TitleBarResult bar = TitleBar(app, "UI gallery", "v0.0.1", true);
		if (bar.minimise) app.Minimize();
		if (bar.maximise) app.ToggleMaximize();
		if (bar.close)    app.Close();
		if (bar.theme)    app.SetTheme(!app.IsLight());

		// Everything but the title bar goes inert behind a dialog.
		ImGui::BeginDisabled(g_dialog);

		// ---- the city map, as the launcher's left column uses it -----------
		const Rect mapBox{ImVec2(0, 40), ImVec2(320, 260)};
		// The gallery can force the motion on. The shipped apps cannot: when
		// the OS says no animation, DESIGN §5 says the map holds still.
		CityMap::Draw(ImGui::GetWindowDrawList(), mapBox.pos, mapBox.size, theme, MapRoute{},
		              app.Seconds() * (g_fastMap ? 40.0f : 1.0f), app.Animates() || g_forceMotion);
		CityMap::DrawFade(ImGui::GetWindowDrawList(), mapBox.pos, mapBox.size, theme,
		                  MapFade{true, 0.35f, 0.92f, 0});
		LogoWidth(ImVec2(40, 84), 216.0f);
		Text(ImVec2(40, 250), Type::Hint, theme.textTertiary, "City map, 320 s drift");

		float x = 360.0f;
		float y = 64.0f;

		Section(ImVec2(x, y), "Buttons", theme);
		Checkbox("##live", ImVec2(x + 120, y + 1), &g_live, "enabled", theme);
		y += 28.0f;
		PrimaryButton("##start", {ImVec2(x, y), ImVec2(240, 60)}, "Start GTA III", Icon::Play,
		              g_live, theme);
		PrimaryButton("##startoff", {ImVec2(x + 256, y), ImVec2(240, 60)}, "Start GTA III",
		              Icon::Play, !g_live, theme);
		y += 72.0f;
		OutlineButton("##again", {ImVec2(x, y), ImVec2(140, 44)}, "Check again", Icon::RotateCcw,
		              theme);
		OutlineButton("##browse", {ImVec2(x + 152, y), ImVec2(120, 44)}, "Browse", Icon::Folder,
		              theme);
		OutlineButton("##options", {ImVec2(x + 284, y), ImVec2(120, 44)}, "Options", Icon::Gear,
		              theme, true);
		IconButton("##copy", {ImVec2(x + 416, y + 4), ImVec2(36, 36)}, Icon::Copy, theme,
		           theme.textButton, 16.0f, true, theme.bgSubtle);
		y += 64.0f;

		Section(ImVec2(x, y), "Input", theme);
		y += 28.0f;
		Field("##name", ImVec2(x, y), 260.0f, "Your name", "4 / 23", g_name, sizeof(g_name),
		      theme);
		Field("##host", ImVec2(x + 276, y), 200.0f, "Server address", nullptr, g_host,
		      sizeof(g_host), theme);
		Field("##port", ImVec2(x + 492, y), 112.0f, "Port", nullptr, g_port, sizeof(g_port),
		      theme);
		y += 92.0f;

		Checkbox("##shortcut", ImVec2(x, y), &g_checked, "Create a desktop shortcut", theme);
		Switch("##ff", ImVec2(x + 300, y - 5), &g_friendly, theme);
		Text(ImVec2(x + 356, y - 1), Type::Body, theme.textPrimary, "Friendly fire");
		y += 42.0f;
		Segmented("##wanted", {ImVec2(x, y), ImVec2(280, 40)}, kWanted, 3, &g_wanted, theme);
		Segmented("##filter", {ImVec2(x + 300, y), ImVec2(200, 40)}, kFilters, 3, &g_filter,
		          theme);
		y += 60.0f;

		Section(ImVec2(x, y), "Status", theme);
		y += 28.0f;
		Card({ImVec2(x, y), ImVec2(420, 44 * 2 + 63)}, theme);
		StatusRow({ImVec2(x, y), ImVec2(420, 44)}, StatusKind::Ok, "gta3.exe is v1.0 retail",
		          nullptr, nullptr, theme);
		StatusRow({ImVec2(x, y + 44), ImVec2(420, 44)}, StatusKind::Ok, "ASI loader found",
		          nullptr, "dinput8.dll", theme);
		StatusRow({ImVec2(x, y + 88), ImVec2(420, 63)}, StatusKind::Fail,
		          "CoopIII.asi is not installed",
		          "Copy it, and CoopIII.ini, next to gta3.exe.", nullptr, theme);
		Pill(ImVec2(x + 440, y + 6), "Running", theme.statusOk, theme.okPillBg,
		     theme.okPillBorder, true);
		Chip(ImVec2(x + 440, y + 44), "Required", theme);
		ProgressBar({ImVec2(x + 440, y + 84), ImVec2(200, 8)}, g_progress, theme, "##bar");
		TextF(ImVec2(x + 440, y + 100), Type::Hint, theme.textTertiary, "%.0f%%",
		      g_progress * 100.0f);
		// Something to drive the bar with, so the easing and the sheen can be
		// seen doing their job.
		if (IconButton("##less", {ImVec2(x + 440, y + 120), ImVec2(30, 30)}, Icon::Minus, theme,
		               theme.textButton, 14.0f))
			g_progress = g_progress > 0.1f ? g_progress - 0.1f : 0.0f;
		if (IconButton("##more", {ImVec2(x + 476, y + 120), ImVec2(30, 30)}, Icon::Plus, theme,
		               theme.textButton, 14.0f))
			g_progress = g_progress < 0.9f ? g_progress + 0.1f : 1.0f;
		y += 190.0f;

		Section(ImVec2(x, y), "Console", theme);
		if (OutlineButton("##say", {ImVec2(x + 540, y - 6), ImVec2(120, 30)}, "Add a line",
		                  Icon::Plus, theme, false, Type::Hint))
			++g_said;
		y += 28.0f;
		std::vector<ConsoleLine> lines = {
		    {"21:29:04", "[coopiii]", "listening on 2001, 8 slots", theme.textTertiary,
		     theme.textSecondary},
		    {"21:31:47", "[coopiii]", "noxx joined (slot 0, net 1)", theme.statusOk,
		     theme.textPrimary},
		    {"21:35:40", "[chat]", "marta: meet at the docks", theme.actionBg, theme.textPrimary},
		    {"21:58:51", "[coopiii]", "rejected peer 3 (reason 1: bad version)", theme.statusWarn,
		     theme.textPrimary},
		};
		for (int i = 0; i < g_said; ++i) {
			char when[16];
			std::snprintf(when, sizeof(when), "22:%02d:%02d", (i * 7) % 60, (i * 23) % 60);
			lines.push_back({when, "[coopiii]", kSayings[i % 5], theme.textTertiary,
			                 theme.textSecondary});
		}
		Card({ImVec2(x, y), ImVec2(660, 120)}, theme, radius::kCard, theme.bgConsole);
		Console({ImVec2(x, y), ImVec2(660, 120)}, lines, theme, g_said > 0, "##gallerylog");
		y += 140.0f;

		// ---- the stepper, down the left under the map ----------------------
		Stepper(ImVec2(40, 320), kSteps, 4, g_step, theme);
		if (OutlineButton("##back", {ImVec2(40, 560), ImVec2(96, 36)}, "Back", Icon::None, theme,
		                  false, Type::BodySmall) &&
		    g_step > 0)
			--g_step;
		if (OutlineButton("##next", {ImVec2(144, 560), ImVec2(96, 36)}, "Next", Icon::None, theme,
		                  false, Type::BodySmall) &&
		    g_step < 3)
			++g_step;

		if (OutlineButton("##dialog", {ImVec2(40, 606), ImVec2(200, 44)}, "Open a dialog",
		                  Icon::Info, theme))
			g_dialog = true;

		Checkbox("##motion", ImVec2(40, 664), &g_forceMotion, "Force the ambient motion on", theme);
		Checkbox("##fast", ImVec2(40, 694), &g_fastMap, "...40x, to see it loop", theme);
		Text(ImVec2(40, 732), Type::Hint, theme.textTertiary,
		     app.Animates() ? "Windows: animation on"
		                    : "Windows: animation off (the OS asked)");
		Text(ImVec2(40, 752), Type::Hint, theme.textTertiary,
		     app.IsLight() ? "Theme: light" : "Theme: dark");

		// The dialog arrives and leaves; while it is leaving it is still on
		// screen, so nothing in it may be pressed.
		ImGui::EndDisabled();

		const float dialogT = Appear("##gallerydialog", g_dialog);
		if (dialogT > 0.0f) {
			const Dialog dialog(screen, ImVec2(480, 260), theme, dialogT, 40.0f);
			ImGui::BeginDisabled(!g_dialog);
			const Rect box = dialog.box;

			Card(box, theme, radius::kDialog, theme.bgPanel);
			Text(Add(box.pos, ImVec2(28, 26)), Type::DialogHeading, theme.textPrimary, "Options");
			TextWrapped(Add(box.pos, ImVec2(28, 70)), box.size.x - 56.0f, Type::Body,
			            theme.textSecondary,
			            "A dialog sits on a scrim, on bg.panel, with a 12 px radius.");
			Switch("##dlgff", Add(box.pos, ImVec2(28, 120)), &g_missionOff, theme);
			Text(Add(box.pos, ImVec2(88, 124)), Type::Body, theme.textPrimary,
			     "Mission fails on death");
			if (OutlineButton("##dlgcancel",
			                  {Add(box.pos, ImVec2(box.size.x - 232, box.size.y - 68)),
			                   ImVec2(104, 44)},
			                  "Cancel", Icon::Close, theme))
				g_dialog = false;
			if (PrimaryButton("##dlgsave",
			                  {Add(box.pos, ImVec2(box.size.x - 120, box.size.y - 68)),
			                   ImVec2(92, 44)},
			                  "Save", Icon::Check, true, theme, Type::ButtonSmall))
				g_dialog = false;
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				g_dialog = false;

			ImGui::EndDisabled();
			dialog.End();
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	});
}
