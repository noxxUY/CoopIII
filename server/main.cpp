// CoopIII dedicated server.
//
//   server.exe [port] [-friendlyfire] [--nogui]
//
// The window by default (design/screens/Server.dc.html); the console with
// --nogui, which is what a dedicated box wants - redirectable output and
// Ctrl-C. Both run the same class in the same process, so there is one
// server and two ways of looking at it.
//
// Settings come from CoopIII-Server.ini next to the executable, and an
// argument wins over the file so a second server on one machine can be
// started on another port without editing anything.
//
// Linked as a Windows application, so --nogui has to go and find a console.
// ui/console.h says what that costs: the server is still running when cmd.exe
// prints its prompt over it, and Ctrl-C still stops it.
#include "run.h"

#include "ui/console.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace coopiii;

namespace {

bool OpenConsole() { return ui::OpenConsole(L"CoopIII Server"); }

void Usage() {
	std::printf("usage: server [port] [-friendlyfire] [-ammosync] [-money off|own|shared]\n"
	            "              [-password PW] [--nogui]\n"
	            "\n"
	            "  port           listen on this UDP port instead of the one in the ini\n"
	            "  -friendlyfire  players can hurt each other (also -ff)\n"
	            "  -ammosync      a player's real ammunition is reported to everyone\n"
	            "                 else instead of a fixed thousand rounds (also -ammo)\n"
	            "  -money RULE    off: each game pays its own player, as always\n"
	            "                 own: rewards go to whoever earned them\n"
	            "                 shared: one wallet for the whole session\n"
	            "                 (also -money=RULE)\n"
	            "  -password PW   players have to give PW to join (also -password=PW)\n"
	            "  --nogui        no window: log to this console, Ctrl-C to stop\n");
}

bool Matches(const char *arg, const char *a, const char *b = nullptr) {
	return std::strcmp(arg, a) == 0 || (b && std::strcmp(arg, b) == 0);
}

} // namespace

int main(int argc, char **argv) {
	Startup startup;
	startup.configPath = ServerConfig::Path();
	startup.hadConfig  = startup.config.Load(startup.configPath);

	// The port stays positional because it always has been. Anything starting
	// with a dash is an option, so `atoi` never gets handed one and quietly
	// turns it into port 0.
	for (int i = 1; i < argc; ++i) {
		if (argv[i][0] != '-') {
			startup.config.port  = static_cast<uint16_t>(std::atoi(argv[i]));
			startup.portFromArgs = true;
			continue;
		}
		if (Matches(argv[i], "-friendlyfire", "-ff")) {
			startup.config.friendlyFire = true;
			continue;
		}
		if (Matches(argv[i], "-ammosync", "-ammo")) {
			startup.config.ammoSync = true;
			continue;
		}
		// The one option with a value. Either `-money shared` or
		// `-money=shared`; a missing or unknown value is refused below like
		// any other bad argument rather than quietly left at off.
		if (Matches(argv[i], "-money") || std::strncmp(argv[i], "-money=", 7) == 0) {
			const char *value = argv[i][6] == '=' ? argv[i] + 7
			                    : i + 1 < argc    ? argv[++i]
			                                      : "";
			MoneyMode rule = startup.config.money;
			if (ParseMoney(value, &rule)) {
				startup.config.money = rule;
				continue;
			}
			OpenConsole();
			std::printf("server: -money takes off, own or shared, not \"%s\"\n\n", value);
			Usage();
			return 2;
		}
		// `-password secret` or `-password=secret`, over the ini's.
		if (Matches(argv[i], "-password") || std::strncmp(argv[i], "-password=", 10) == 0) {
			const char *value = argv[i][9] == '=' ? argv[i] + 10
			                    : i + 1 < argc    ? argv[++i]
			                                      : "";
			startup.config.password = ServerConfig::CleanPassword(value);
			continue;
		}
		// Both spellings: the options this already had use one dash, and
		// turning somebody away over a dash they did not type is not worth
		// being right about.
		if (Matches(argv[i], "--nogui", "-nogui")) {
			startup.gui = false;
			continue;
		}
		if (Matches(argv[i], "--gui", "-gui")) {
			startup.gui = true;
			continue;
		}
		if (Matches(argv[i], "--help", "-h") || Matches(argv[i], "-?", "/?")) {
			OpenConsole();
			Usage();
			return 0;
		}

		OpenConsole();
		std::printf("server: %s is not an option\n\n", argv[i]);
		Usage();
		return 2;
	}

	if (!startup.gui) {
		if (!OpenConsole()) {
			MessageBoxW(nullptr,
			            L"CoopIII Server was started with --nogui but could not open a "
			            L"console to write to.",
			            L"CoopIII Server", MB_ICONERROR | MB_OK);
			return 1;
		}
		return RunConsole(startup);
	}

	return RunWindow(startup);
}
