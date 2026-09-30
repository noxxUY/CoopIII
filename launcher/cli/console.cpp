// CoopIII launcher, the console front end: `coopiii-launcher --nogui`.
//
// Job here is to catch the things that can go wrong before the game even
// starts, and fail here with a plain sentence, rather than twenty seconds
// later inside some hook:
//
//   1. the game is not where we think it is
//   2. the exe is not the build CoopIII targets
//   3. there is no ASI loader, so CoopIII.asi would never be loaded at all
//   4. CoopIII.asi is missing, or is an older build than this launcher
//   5. the player has not said which server to join
//
// Does NOT inject itself. Player already has an ASI loader
// (docs/compat.md §2.1), and letting that do the loading keeps CoopIII a
// well-behaved guest next to their other mods. So this just checks things,
// reports what it found, and starts the game.
//
// The checks, the ini writing and the start all live in launcher-core, which
// the window uses too - so the two can never disagree about what a good
// install looks like. The arguments are read in launcher/main.cpp.
#include "run.h"

#include "launcher/core.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace coopiii::launcher {
namespace {

void Say(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::printf("\n");
}

void Report(const Check &check) {
	switch (check.state) {
	case Check::State::Ok:
		Say("  [ ok ] %s%s%s", check.title.c_str(), check.meta.empty() ? "" : " - ",
		    check.meta.c_str());
		if (!check.detail.empty())
			Say("         %s", check.detail.c_str());
		break;
	case Check::State::Info:
		Say("  [info] %s", check.title.c_str());
		break;
	case Check::State::Fail:
		Say("  [FAIL] %s", check.title.c_str());
		if (!check.detail.empty())
			Say("         %s", check.detail.c_str());
		if (!check.link.empty())
			Say("         %s", check.link.c_str());
		if (!check.mono.empty())
			Say("         %s %s", check.monoLabel.c_str(), check.mono.c_str());
		break;
	}
}

} // namespace

int RunConsole(const Startup &startup) {
	std::string gameDir = startup.gameDir;

	Say("CoopIII launcher");
	Say("");

	if (gameDir.empty()) {
		gameDir = FindGameDir();
		if (gameDir.empty()) {
			Say("Could not find GTA III.");
			Say("Point at it:  coopiii-launcher --nogui --game \"C:\\path\\to\\GTA3\"");
			return 1;
		}
	}
	if (!DirExists(gameDir)) {
		Say("No such directory: %s", gameDir.c_str());
		return 1;
	}

	Say("game: %s", Join(gameDir, "gta3.exe").c_str());
	const Checks checks = RunChecks(gameDir);
	for (const Check &c : checks.items)
		Report(c);

	if (!startup.host.empty() || !startup.nick.empty()) {
		const std::string clean = SanitizeNick(startup.nick);
		if (!startup.nick.empty() && clean != startup.nick)
			Say("  [warn] name trimmed to \"%s\" (23 characters travel on the wire)",
			    clean.c_str());

		const std::string ini = Join(gameDir, "CoopIII.ini");
		if (UpdateIni(ini, startup.host, startup.port, clean))
			Say("  [ ok ] updated %s", ini.c_str());
		else
			Say("  [warn] could not write CoopIII.ini");
	}

	Say("");
	if (checks.problems > 0) {
		Say("%d problem%s, not starting the game.", checks.problems,
		    checks.problems == 1 ? "" : "s");
		Say("Fix the [FAIL] lines above and run this again.");
		return 1;
	}
	Say("Everything checks out.");

	if (startup.check)
		return 0;

	Say("Starting GTA III...");
	std::string error;
	if (!LaunchGame(gameDir, &error)) {
		Say("%s", error.c_str());
		return 1;
	}

	Say("");
	Say("If anything goes wrong, read CoopIII.log in the game folder for what "
	    "the client did and why it stopped.");
	return 0;
}

} // namespace coopiii::launcher
