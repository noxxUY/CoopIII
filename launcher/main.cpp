// CoopIII launcher.
//
//   coopiii-launcher [--game <dir>] [--server <host[:port]>] [--nick <name>]
//                    [--check] [--nogui]
//
// The window by default (design/screens/Main.dc.html); the console with
// --nogui, for a shortcut that should go straight into the game or a script
// that wants the checks as text. Both are launcher-core underneath.
//
// Arguments work in both. In the window they fill the form in over what
// CoopIII.ini says, so a shortcut can carry a server and a name and still show
// the checks before anything starts.
//
// Linked as a Windows application, so --nogui has to go and find a console;
// ui/console.h says what that costs.
#include "run.h"

#include "launcher/core.h"
#include "ui/console.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace coopiii::launcher;

namespace {

bool OpenConsole() { return ui::OpenConsole(L"CoopIII Launcher"); }

void Usage() {
	std::printf("usage: coopiii-launcher [options]\n"
	            "\n"
	            "  --game <dir>            where GTA III is installed\n"
	            "  --server <host[:port]>  server to join\n"
	            "  --nick <name>           your name in game\n"
	            "  --check                 run the checks and stop (implies --nogui)\n"
	            "  --nogui                 no window: check, write CoopIII.ini, start the game\n"
	            "\n"
	            "With no arguments it opens the window.\n");
}

bool Matches(const char *arg, const char *a, const char *b = nullptr) {
	return std::strcmp(arg, a) == 0 || (b && std::strcmp(arg, b) == 0);
}

// Refuses at the door, in whichever console there is, and says why.
int Refuse(const char *fmt, const char *what) {
	OpenConsole();
	std::printf("coopiii-launcher: ");
	std::printf(fmt, what);
	std::printf("\n\n");
	Usage();
	return 2;
}

} // namespace

int main(int argc, char **argv) {
	Startup startup;

	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		// The three that take a value. A missing value is a mistake worth
		// stopping on: `--game` on its own at the end of a shortcut would
		// otherwise quietly mean "find it yourself".
		const bool takesValue = Matches(a, "--game") || Matches(a, "--server") ||
		                        Matches(a, "--nick");
		if (takesValue && i + 1 >= argc)
			return Refuse("%s needs a value", a);

		if (Matches(a, "--game")) {
			startup.gameDir = argv[++i];
		} else if (Matches(a, "--server")) {
			const std::string server = argv[++i];
			const size_t      colon  = server.find(':');
			startup.host             = server.substr(0, colon);
			if (colon != std::string::npos &&
			    !ParsePort(server.substr(colon + 1), &startup.port))
				return Refuse("%s is not a port", server.c_str() + colon + 1);
			if (!ValidHost(startup.host))
				return Refuse("%s is not a server address", startup.host.c_str());
		} else if (Matches(a, "--nick")) {
			startup.nick = argv[++i];
		} else if (Matches(a, "--check", "--no-launch")) {
			startup.check = true;
			startup.gui   = false;
		} else if (Matches(a, "--nogui", "-nogui")) {
			startup.gui = false;
		} else if (Matches(a, "--gui", "-gui")) {
			startup.gui = true;
		} else if (Matches(a, "--help", "-h") || Matches(a, "-?", "/?")) {
			OpenConsole();
			Usage();
			return 0;
		} else {
			return Refuse("%s is not an option", a);
		}
	}

	if (!startup.gui) {
		if (!OpenConsole()) {
			MessageBoxW(nullptr,
			            L"CoopIII Launcher was started with --nogui but could not open a "
			            L"console to write to.",
			            L"CoopIII Launcher", MB_ICONERROR | MB_OK);
			return 1;
		}
		const int result = RunConsole(startup);

		// A --nogui shortcut gets a console of its own, and that console goes
		// the moment this returns - taking with it the one line that says why
		// the game did not start. Only then: a console somebody typed into is
		// still there afterwards, and a script must not be left waiting.
		DWORD attached[2];
		if (result != 0 && GetConsoleProcessList(attached, 2) == 1) {
			std::printf("\nPress Enter to close.");
			std::getchar();
		}
		return result;
	}

	return RunWindow(startup);
}
