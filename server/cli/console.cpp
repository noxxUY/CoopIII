// The console front end: stdout, a signal handler, and nothing else.
//
// Reached with `server --nogui`, which is how a dedicated box runs it - no
// window, output that can be redirected to a file, and Ctrl-C to stop. The
// server itself is server/core/server.h, the same class the window runs.
#include "run.h"

#include "server.h"

#include <csignal>
#include <cstdio>

namespace coopiii {
namespace {

volatile std::sig_atomic_t g_running = 1;

void OnSignal(int) { g_running = 0; }

} // namespace

int RunConsole(const Startup &startup) {
	// Unbuffered, so logs show up when stdout is redirected to a file or a
	// pipe and the process is killed rather than exiting cleanly.
	//
	// This said `_IOLBF` and did nothing. The MSVC CRT documents `_IOLBF` as
	// meaning full buffering on Win32 - it is accepted, it is not line
	// buffering, and the only difference from the default is the buffer size.
	// Measured: a server started with `> server.log`, driven through a whole
	// session and then killed leaves an empty file, while the same server run
	// to a console prints everything. Every diagnostic the server has was
	// invisible to anyone capturing its output, which for a dedicated server
	// is everyone.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);

	if (startup.hadConfig)
		std::printf("[coopiii] settings from %s\n", startup.configPath.c_str());
	else
		std::printf("[coopiii] no %s, using the defaults\n", startup.configPath.c_str());
	std::printf("[coopiii] wanted level %s, mission fails on death %s, rampages %s, "
	            "cheats %s, money %s\n",
	            Name(startup.config.wantedLevel),
	            startup.config.missionFailOnDeath ? "on" : "off",
	            Name(startup.config.rampage), Name(startup.config.cheats),
	            Name(startup.config.money));

	Server server;
	if (!server.Start(startup.config.port, startup.config.friendlyFire,
	                  startup.config.ammoSync, WireValue(startup.config.wantedLevel),
	                  WireValue(startup.config.rampage),
	                  WireValue(startup.config.cheats),
	                  WireValue(startup.config.money)))
		return 1;

	while (g_running)
		server.Tick();

	std::printf("[coopiii] shutting down\n");
	server.Stop();
	return 0;
}

} // namespace coopiii
