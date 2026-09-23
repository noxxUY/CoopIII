// CoopIII-Server.ini: what it parses, what it writes, and that a round trip
// through both comes back with the same rules.
//
// The defaults are the part worth a test of their own. docs/roadmap.md §5.5
// says single-player behaviour wins where it and co-op convenience disagree,
// and §5.1, §5.2 and §5.4 are that policy applied - a default that quietly
// drifts is a settled decision being reopened by accident.
#include "config.h"

#include <cstdio>
#include <string>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what) {
	std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
	if (!ok)
		++g_failures;
}

void TestDefaults() {
	std::printf("defaults (roadmap 5)\n");
	const ServerConfig c;
	Check(c.port == 2001, "the port is 2001");
	Check(c.friendlyFire == false, "friendly fire is off (5.2)");
	Check(c.wantedLevel == WantedLevelRule::PerPlayer, "wanted level is per player (5.1)");
	Check(c.missionFailOnDeath == true, "a mission fails on death (5.4)");
	Check(c.ammoSync == false, "ammo sync is off (protocol 1.9.6)");
}

void TestParse() {
	std::printf("parsing\n");

	ServerConfig c;
	Check(c.Parse("[CoopIII]\nport = 2010\nfriendlyFire = true\nwantedLevel = shared\n"
	              "missionFailOnDeath = false\nammoSync = true\n"),
	      "a full file parses");
	Check(c.port == 2010, "port");
	Check(c.friendlyFire, "friendly fire");
	Check(c.wantedLevel == WantedLevelRule::Shared, "wanted level");
	Check(!c.missionFailOnDeath, "mission fails on death");
	Check(c.ammoSync, "ammo sync");

	// Keys are matched without case, values too, and yes/on/1 all mean true -
	// a config file is edited by hand and should not be fussy.
	ServerConfig loose;
	loose.Parse("PORT=2020\nFRIENDLYFIRE = YES\nwantedlevel=OFF\nmissionfailondeath = 0\n"
	            "AMMOSYNC = on\n");
	Check(loose.port == 2020, "an uppercase key still parses");
	Check(loose.friendlyFire, "\"YES\" is true");
	Check(loose.wantedLevel == WantedLevelRule::Off, "\"OFF\" is the off rule");
	Check(!loose.missionFailOnDeath, "\"0\" is false");
	Check(loose.ammoSync, "\"on\" is true for ammo sync too");

	// A value that makes no sense leaves the setting where it was, rather
	// than silently becoming something else.
	ServerConfig keep;
	keep.Parse("port = 0\nport = 99999\nwantedLevel = sideways\n");
	Check(keep.port == 2001, "an out-of-range port is ignored");
	Check(keep.wantedLevel == WantedLevelRule::PerPlayer,
	      "an unknown wanted level is ignored");

	// Comments, blank lines and keys from a newer build.
	ServerConfig future;
	Check(future.Parse("; a comment\n\n[CoopIII]\nport = 2005\n"
	                   "somethingNewerKnowsAbout = 7\n"),
	      "a file with comments and an unknown key parses");
	Check(future.port == 2005, "and the key it did know still landed");

	Check(!ServerConfig{}.Parse(""), "an empty file parses as nothing");
}

void TestRoundTrip() {
	std::printf("round trip\n");

	ServerConfig written;
	written.port               = 2345;
	written.friendlyFire       = true;
	written.wantedLevel        = WantedLevelRule::Off;
	written.missionFailOnDeath = false;
	written.ammoSync           = true;

	const std::string ini = written.ToIni();
	Check(ini.find("[CoopIII]") != std::string::npos, "the file has its section header");
	Check(ini.find(';') != std::string::npos, "and comments explaining the keys");

	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "what was written reads back the same");

	// And the defaults survive the trip too, which is the case that actually
	// ships.
	ServerConfig fresh;
	ServerConfig back;
	back.Parse(fresh.ToIni());
	Check(back == fresh, "the defaults round trip");
}

void TestNames() {
	std::printf("names\n");
	Check(std::string(Name(WantedLevelRule::PerPlayer)) == "perplayer", "perplayer");
	Check(std::string(Name(WantedLevelRule::Shared)) == "shared", "shared");
	Check(std::string(Name(WantedLevelRule::Off)) == "off", "off");
	Check(std::string(Label(WantedLevelRule::PerPlayer)) == "Per player",
	      "the window says \"Per player\"");

	WantedLevelRule rule = WantedLevelRule::Shared;
	Check(ParseWantedLevel("per-player", &rule) && rule == WantedLevelRule::PerPlayer,
	      "\"per-player\" with a dash is accepted too");
	Check(!ParseWantedLevel("", &rule), "an empty rule is refused");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	TestDefaults();
	TestParse();
	TestRoundTrip();
	TestNames();

	std::printf("\n%s\n", g_failures == 0 ? "all server config checks passed"
	                                      : "server config checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
