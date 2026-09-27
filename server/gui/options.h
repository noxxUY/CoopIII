// The server window's options dialog: every setting in CoopIII-Server.ini,
// in sections, with what each one does under its name.
//
// design/screens/ServerOptions.dc.html is where the dialog's look comes from:
// its header, its rows of a title and a line under it with the control on the
// right, and its footer. It has grown past the three rules that screen shows,
// so it scrolls, and a rail on the left jumps between the sections.
#pragma once

#include "config.h"

#include "ui/app.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace coopiii {

enum OptionsSection : int {
	SECTION_SERVER,
	SECTION_PLAYERS,
	SECTION_WANTED,
	SECTION_MISSIONS,
	SECTION_MONEY,
	SECTION_WORLD,
	SECTION_COUNT
};

// What the dialog keeps between frames.
struct OptionsDialog {
	bool         open = false;
	ServerConfig editing;
	char         port[8]                = {};
	char         password[PASSWORD_LEN] = {};
	float        scroll                 = 0.0f;   // where the list is headed
	float        contentH               = 0.0f;   // the whole list, last frame
	float        sectionY[SECTION_COUNT] = {};    // where each section starts in it
	bool         dragging               = false;  // the scroll bar's thumb is held
	float        dragFrom               = 0.0f;

	// Opens it on the settings in force.
	void Open(const ServerConfig &from);
};

enum class OptionsResult { None, Save, Close };

// Draws the dialog while it is open or on its way out. On Save, `editing` is
// what to keep; the caller saves the file and applies it.
OptionsResult DrawOptions(OptionsDialog &dialog, const ServerConfig &saved, ui::App &app,
                          ImVec2 screen, float topInset);

// What differs between two configs, one line each, for the console. The
// password is said to have changed, never what it is.
std::vector<std::string> DescribeChanges(const ServerConfig &before, const ServerConfig &after);

// The rules the main screen's card shows, as many as fit, most asked-about
// first.
struct RuleSummary {
	const char *label;
	std::string value;
};
std::vector<RuleSummary> SummariseRules(const ServerConfig &config);

// How many settings the dialog has.
size_t SettingCount();

} // namespace coopiii
