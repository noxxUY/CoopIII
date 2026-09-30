// The two front ends over launcher-core, and what coopiii-launcher.exe hands
// them.
//
// One executable, two ways to run it: the window by default, the console with
// --nogui. The checks, the ini and the start are launcher-core either way, so
// the two cannot disagree about what a good install looks like.
#pragma once

#include <cstdint>
#include <string>

namespace coopiii::launcher {

// What the command line asked for. Empty means "not given": the window fills
// those in from CoopIII.ini and the game it finds, the console leaves the ini
// alone.
struct Startup {
	std::string gameDir;
	std::string host;
	uint16_t    port  = 0;
	std::string nick;
	bool        check = false;   // run the checks and stop; console only
	bool        gui   = true;
};

// launcher/gui/window.cpp - design/screens/Main.dc.html.
int RunWindow(const Startup &startup);

// launcher/cli/console.cpp - the checks as text, then the game.
int RunConsole(const Startup &startup);

} // namespace coopiii::launcher
