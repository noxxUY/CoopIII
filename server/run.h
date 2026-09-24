// The two front ends over server-core, and what server.exe hands them.
//
// One executable, two ways to run it: the window by default, the console with
// --nogui. The session itself is the same class either way (server/core/
// server.h), so the two cannot disagree about what a session is doing - the
// only thing a front end supplies is where the log lines go.
#pragma once

#include "config.h"
#include "reach.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coopiii {

// What the command line and CoopIII-Server.ini between them decided, before
// either front end starts.
struct Startup {
	ServerConfig config;
	std::string  configPath;
	bool         hadConfig = false;   // the ini existed and was read
	bool         portFromArgs = false;
	bool         gui = true;
};

// server/gui/window.cpp - design/screens/Server.dc.html.
int RunWindow(const Startup &startup);

// server/cli/console.cpp - stdout and a signal handler.
int RunConsole(const Startup &startup);

// server/addresses.cpp - this machine's IPv4 addresses, host order, loopback
// and tunnels left out, each with whether its adapter has a gateway. For
// server/core/reach.h.
std::vector<LocalAddress> LocalIPv4Addresses();

} // namespace coopiii
