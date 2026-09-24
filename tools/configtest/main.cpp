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
	// roadmap.md 5.10 decided shared, so shared is the default. The other two
	// values are a group disagreeing about difficulty, not about the rule.
	Check(c.rampage == RampageMode::Shared, "rampages are shared (5.10)");
	// Every cheat works in single player, so every cheat works by default
	// (5.5, 5.14); the other two values are a group choosing otherwise.
	Check(c.cheats == CheatMode::Shared, "cheats are shared (5.14)");
	// Every build before the setting existed paid each machine's own player
	// for what its own engine saw, so that is what off is and off is the
	// default.
	Check(c.money == MoneyMode::Off, "money is off");
	Check(c.hiddenPackages == PackageMode::Shared, "hidden packages are shared (5.11)");
}

void TestPassword() {
	std::printf("the password\n");

	ServerConfig c;
	Check(c.password.empty(), "none by default: anybody with the address can join");
	Check(c.Parse("password = hunter2\n") && c.password == "hunter2", "a password parses");
	c.Parse("password = \n");
	Check(c.password.empty(), "and an empty one is none again");
	Check(ServerConfig::CleanPassword("a\tb\x01" "c") == "abc",
	      "control characters are left out, as a hello cannot carry them");
	Check(ServerConfig::CleanPassword(std::string(60, 'x')).size() == PASSWORD_LEN - 1,
	      "and one longer than the packet is cut to what it carries");

	ServerConfig written;
	written.password      = "open sesame";
	written.hiddenPackages = PackageMode::PerPlayer;
	const std::string ini = written.ToIni();
	Check(ini.find("password = open sesame\n") != std::string::npos,
	      "the file says the password, on the last line and whole");
	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "and reads back the same");
	ServerConfig other = written;
	other.password     = "";
	Check(other != written, "and a different password is a different config");
}

void TestHiddenPackages() {
	std::printf("hidden packages (5.11)\n");

	ServerConfig c;
	Check(c.Parse("hiddenPackages = perplayer\n") && c.hiddenPackages == PackageMode::PerPlayer,
	      "\"perplayer\" parses");
	Check(c.Parse("HIDDENPACKAGES = Shared\n") && c.hiddenPackages == PackageMode::Shared,
	      "\"Shared\" parses, key and value both without case");
	Check(c.Parse("hiddenPackages = per-player\n") && c.hiddenPackages == PackageMode::PerPlayer,
	      "and so does the spelling the wanted level takes");
	c.Parse("hiddenPackages = some\n");
	Check(c.hiddenPackages == PackageMode::PerPlayer, "an unknown value leaves it where it was");

	Check(std::string(Name(PackageMode::Shared)) == "shared" &&
	          std::string(Name(PackageMode::PerPlayer)) == "perplayer",
	      "the two names are the two values the file takes");
	Check(WireValue(PackageMode::Shared) == PACKAGES_SHARED &&
	          WireValue(PackageMode::PerPlayer) == PACKAGES_PERPLAYER,
	      "and each one is the session's rule");

	ServerConfig written;
	written.hiddenPackages = PackageMode::PerPlayer;
	const std::string ini  = written.ToIni();
	Check(ini.find("hiddenPackages = perplayer") != std::string::npos,
	      "the file says hiddenPackages = perplayer");
	Check(ini.find("money = off") != std::string::npos,
	      "and still has the key written before it, whole");
	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "and reads back the same");
	ServerConfig other = written;
	other.hiddenPackages = PackageMode::Shared;
	Check(other != written, "and a different package rule is a different config");
}

void TestMoney() {
	std::printf("money\n");

	ServerConfig c;
	Check(c.Parse("money = own\n") && c.money == MoneyMode::Own, "\"own\" parses");
	Check(c.Parse("MONEY = Shared\n") && c.money == MoneyMode::Shared,
	      "\"Shared\" parses, key and value both without case");
	Check(c.Parse("money = off\n") && c.money == MoneyMode::Off, "\"off\" parses");
	Check(c.Parse("money = pooled\n") && c.money == MoneyMode::Shared,
	      "\"pooled\" is another word for shared");
	c.Parse("money = everybody's\n");
	Check(c.money == MoneyMode::Shared, "an unknown value leaves it where it was");

	MoneyMode m = MoneyMode::Off;
	Check(ParseMoney("  own ", &m) && m == MoneyMode::Own, "the value is trimmed, as on the command line");
	Check(!ParseMoney("", &m) && m == MoneyMode::Own, "an empty value is refused and changes nothing");

	Check(std::string(Name(MoneyMode::Off)) == "off" &&
	          std::string(Name(MoneyMode::Own)) == "own" &&
	          std::string(Name(MoneyMode::Shared)) == "shared",
	      "the three names are the three values the file takes");
	Check(WireValue(MoneyMode::Off) == MONEY_RULE_OFF &&
	          WireValue(MoneyMode::Own) == MONEY_RULE_OWN &&
	          WireValue(MoneyMode::Shared) == MONEY_RULE_SHARED,
	      "and each one is the rule S_Money carries");

	ServerConfig written;
	written.money = MoneyMode::Shared;
	const std::string ini = written.ToIni();
	Check(ini.find("money = shared") != std::string::npos, "the file says money = shared");
	Check(ini.find("cheats = shared") != std::string::npos,
	      "and still has the key written before it, whole");
	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "and reads back the same");
	ServerConfig other = written;
	other.money = MoneyMode::Own;
	Check(other != written, "and a different money rule is a different config");
}

void TestCheats() {
	std::printf("cheats (5.14)\n");

	ServerConfig c;
	Check(c.Parse("cheats = personal\n") && c.cheats == CheatMode::Personal,
	      "\"personal\" parses");
	Check(c.Parse("CHEATS = OFF\n") && c.cheats == CheatMode::Off,
	      "\"OFF\" parses, key and value both without case");
	Check(c.Parse("cheats = shared\n") && c.cheats == CheatMode::Shared,
	      "\"shared\" parses");
	c.cheats = CheatMode::Personal;
	c.Parse("cheats = sometimes\n");
	Check(c.cheats == CheatMode::Personal, "an unknown value leaves it where it was");

	Check(std::string(Name(CheatMode::Shared)) == "shared" &&
	          std::string(Name(CheatMode::Personal)) == "personal" &&
	          std::string(Name(CheatMode::Off)) == "off",
	      "the three names are the three values the file takes");
	Check(WireValue(CheatMode::Shared) == CHEAT_RULE_SHARED &&
	          WireValue(CheatMode::Personal) == CHEAT_RULE_PERSONAL &&
	          WireValue(CheatMode::Off) == CHEAT_RULE_OFF,
	      "and each one is the rule the welcome carries");

	ServerConfig written;
	written.cheats = CheatMode::Off;
	const std::string ini = written.ToIni();
	Check(ini.find("cheats = off") != std::string::npos,
	      "the file says cheats = off");
	Check(ini.find("rampages = shared") != std::string::npos,
	      "and still has the key written before it, whole");
	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "and reads back the same");
}

void TestParse() {
	std::printf("parsing\n");

	ServerConfig c;
	Check(c.Parse("[CoopIII]\nport = 2010\nfriendlyFire = true\nwantedLevel = shared\n"
	              "missionFailOnDeath = false\nammoSync = true\nrampages = scaled\n"),
	      "a full file parses");
	Check(c.port == 2010, "port");
	Check(c.friendlyFire, "friendly fire");
	Check(c.wantedLevel == WantedLevelRule::Shared, "wanted level");
	Check(!c.missionFailOnDeath, "mission fails on death");
	Check(c.ammoSync, "ammo sync");
	Check(c.rampage == RampageMode::Scaled, "the rampage rule");

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
	written.rampage            = RampageMode::Scaled;
	written.money              = MoneyMode::Own;
	written.hiddenPackages     = PackageMode::PerPlayer;

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
	TestCheats();
	TestMoney();
	TestHiddenPackages();
	TestPassword();

	std::printf("\n%s\n", g_failures == 0 ? "all server config checks passed"
	                                      : "server config checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
