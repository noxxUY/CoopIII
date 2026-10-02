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
	Check(c.missionMarginCm == 500, "5 m outside a start or a checkpoint still counts");
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
	      "the file says the password, on a line of its own and whole");
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

void TestCoopCheats() {
	std::printf("CoopIII's own cheats (cheats.md 7)\n");

	ServerConfig c;
	Check(c.coopCheats == CoopCheatMode::OutsideMissions,
	      "by default they work, but not during a mission");
	Check(c.Parse("coopCheats = always\n") && c.coopCheats == CoopCheatMode::Always,
	      "\"always\" parses");
	Check(c.Parse("COOPCHEATS = OFF\n") && c.coopCheats == CoopCheatMode::Off,
	      "\"OFF\" parses, key and value both without case");
	Check(c.Parse("coopCheats = outsidemissions\n") &&
	          c.coopCheats == CoopCheatMode::OutsideMissions,
	      "\"outsidemissions\" parses");
	Check(c.Parse("coopCheats = false\n") && c.coopCheats == CoopCheatMode::Off &&
	          c.Parse("coopCheats = on\n") && c.coopCheats == CoopCheatMode::OutsideMissions,
	      "and false and on mean off and the default");
	c.coopCheats = CoopCheatMode::Always;
	c.Parse("coopCheats = sometimes\n");
	Check(c.coopCheats == CoopCheatMode::Always, "an unknown value leaves it where it was");
	c.Parse("cheats = off\n");
	Check(c.coopCheats == CoopCheatMode::Always, "and `cheats` is a setting of its own");

	Check(std::string(Name(CoopCheatMode::OutsideMissions)) == "outsidemissions" &&
	          std::string(Name(CoopCheatMode::Always)) == "always" &&
	          std::string(Name(CoopCheatMode::Off)) == "off",
	      "the three names are the three values the file takes");
	Check(WireValue(CoopCheatMode::OutsideMissions) == COOP_CHEATS_OUTSIDE_MISSIONS &&
	          WireValue(CoopCheatMode::Always) == COOP_CHEATS_ALWAYS &&
	          WireValue(CoopCheatMode::Off) == COOP_CHEATS_OFF,
	      "and each one is the rule S_SessionRules carries");
	Check(SaneCoopCheatRule(7) == COOP_CHEATS_OUTSIDE_MISSIONS &&
	          SaneCoopCheatRule(COOP_CHEATS_OFF) == COOP_CHEATS_OFF,
	      "a byte that is no rule reads as the default");

	ServerConfig defaults;
	Check(defaults.ToIni().find("coopCheats = outsidemissions") != std::string::npos,
	      "a fresh file says coopCheats = outsidemissions");
	ServerConfig written;
	written.coopCheats = CoopCheatMode::Always;
	const std::string ini = written.ToIni();
	Check(ini.find("coopCheats = always") != std::string::npos, "the file says coopCheats = always");
	Check(ini.find("cheats = shared") != std::string::npos, "beside cheats = shared, whole");
	ServerConfig read;
	read.Parse(ini);
	Check(read == written, "and reads back the same");
	Check(read != defaults, "and a different rule is a different config");
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

	ServerConfig margin;
	margin.Parse("missionMargin = 7.5\n");
	Check(margin.missionMarginCm == 750, "the mission margin is in metres, a fraction allowed");
	margin.Parse("missionMargin = 0\n");
	Check(margin.missionMarginCm == 0, "and 0 is the area exactly as the game has it");
	margin.Parse("missionMargin = -1\nmissionMargin = 80\nmissionMargin = far\n");
	Check(margin.missionMarginCm == 0,
	      "a negative one, one past 50 m and one that is not a number are ignored");

	ServerConfig enemies;
	Check(enemies.missionEnemies == MISSION_ENEMIES_ORIGINAL && enemies.missionScale == 50,
	      "a mission's enemies are single player's unless the server says, at 50% a player");
	enemies.Parse("missionEnemies = tougher\nmissionScale = 75\n");
	Check(enemies.missionEnemies == MISSION_ENEMIES_TOUGHER && enemies.missionScale == 75,
	      "tougher, and 75% for each player after the first");
	enemies.Parse("missionEnemies = harder\nmissionScale = 500\nmissionScale = lots\n");
	Check(enemies.missionEnemies == MISSION_ENEMIES_TOUGHER && enemies.missionScale == 75,
	      "an unknown rule, a scale past 200% and one that is not a number are ignored");
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
	written.missionMarginCm    = 333;
	written.missionEnemies     = MISSION_ENEMIES_MORE;
	written.missionScale       = 120;
	written.maxPlayers              = 5;
	written.maxWanted               = 4;
	written.missionCheckpointWaitS  = 90;
	written.missionTimedCheckpoints = true;
	written.missionCatchUpM         = 120;
	written.missionBehindM          = 220;
	written.missionBehindS          = 12;
	written.missionIntroWaitS       = 45;
	written.missionPayHelpers       = false;
	written.cutsceneSkip            = VoteRule::All;
	written.rampageVote             = VoteRule::Half;
	written.rampageVoteS            = 20;
	written.abandonedCarS           = 90;
	written.cheats                  = CheatMode::Personal;
	written.password                = "letmein";
	written.lookUpPublicAddress     = false;
	written.openRouterPort          = false;
	written.keepProgress            = false;

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

// The settings that were numbers built into the server and the clients until
// the window could change them. Their defaults are those numbers, so a file
// without them is the server it always was.
void TestTheSettingsThatWereBuiltIn() {
	std::printf("the settings that used to be built in\n");

	const ServerConfig c;
	Check(c.maxPlayers == MAX_PLAYERS, "every slot is open");
	Check(c.maxWanted == 6, "six stars, the game's own");
	Check(c.missionCheckpointWaitS == 60 && !c.missionTimedCheckpoints,
	      "a checkpoint waits a minute, and not in a race or against a clock");
	Check(c.missionCatchUpM == 60 && c.missionBehindM == 150 && c.missionBehindS == 10,
	      "latecomers past 60 m and stragglers past 150 m for 10 s are brought along");
	Check(c.missionIntroWaitS == 60, "a start waits a minute for a game in its own intro");
	Check(c.missionPayHelpers, "everybody in a mission is paid (missions.md 12.1)");
	Check(c.cutsceneSkip == VoteRule::Most && c.rampageVote == VoteRule::Most &&
	          c.rampageVoteS == 15,
	      "both votes take 75%, and the rampage vote 15 s");
	Check(c.abandonedCarS == 60, "an abandoned car goes after a minute");

	ServerConfig p;
	Check(p.Parse("maxPlayers = 4\nmaxWantedLevel = 3\nmissionCheckpointWait = 0\n"
	              "missionTimedCheckpoints = yes\nmissionCatchUp = 0\nmissionFallBehind = 400\n"
	              "missionFallBehindTime = 30\nmissionIntroWait = 120\nmissionPayHelpers = false\n"
	              "cutsceneSkip = anyone\nrampageVote = all\nrampageVoteTime = 30\n"
	              "abandonedCars = 300\n"),
	      "every one of them parses");
	Check(p.maxPlayers == 4 && p.maxWanted == 3, "the player limit and the stars");
	Check(p.missionCheckpointWaitS == 0 && p.missionTimedCheckpoints,
	      "checkpoints that never wait, and the timed ones too");
	Check(p.missionCatchUpM == 0 && p.missionBehindM == 400 && p.missionBehindS == 30,
	      "nobody brought in late, stragglers past 400 m for 30 s");
	Check(p.missionIntroWaitS == 120 && !p.missionPayHelpers, "the intro wait and the pay");
	Check(p.cutsceneSkip == VoteRule::Anyone && p.rampageVote == VoteRule::All &&
	          p.rampageVoteS == 30,
	      "the two votes");
	Check(p.abandonedCarS == 300, "and the abandoned cars");

	// Out of range, or not a number: left where it was.
	ServerConfig keep;
	keep.Parse("maxPlayers = 0\nmaxPlayers = 9\nmaxWantedLevel = 0\nmaxWantedLevel = 7\n"
	           "missionCheckpointWait = 601\nmissionCatchUp = -5\nmissionFallBehind = lots\n"
	           "missionFallBehindTime = 0\nmissionIntroWait = 5\nrampageVoteTime = 61\n"
	           "rampageVoteTime = 4\nabandonedCars = 9\nabandonedCars = 601\n"
	           "cutsceneSkip = maybe\nrampageVote = \n");
	Check(keep == ServerConfig{}, "every value out of its range, or not one at all, is ignored");

	ServerConfig spelled;
	spelled.Parse("cutsceneSkip = 50%\nrampageVote = everyone\n");
	Check(spelled.cutsceneSkip == VoteRule::Half && spelled.rampageVote == VoteRule::All,
	      "a vote rule can be written as a share or a word");
	spelled.Parse("cutsceneSkip = MOST\nrampageVote = One\n");
	Check(spelled.cutsceneSkip == VoteRule::Most && spelled.rampageVote == VoteRule::Anyone,
	      "in any case");

	Check(std::string(Name(VoteRule::Half)) == "half" && std::string(Name(VoteRule::Anyone)) == "anyone",
	      "the file says half and anyone");
	Check(std::string(Label(VoteRule::Most)) == "75%", "and the window says 75%");

	// Each of them on its own survives the file, so none is written under
	// the wrong key or read into the wrong field.
	ServerConfig each[13];
	each[0].maxPlayers              = 3;
	each[1].maxWanted               = 2;
	each[2].missionCheckpointWaitS  = 125;
	each[3].missionTimedCheckpoints = true;
	each[4].missionCatchUpM         = 250;
	each[5].missionBehindM          = 0;
	each[6].missionBehindS          = 45;
	each[7].missionIntroWaitS       = 15;
	each[8].missionPayHelpers       = false;
	each[9].cutsceneSkip            = VoteRule::Half;
	each[10].rampageVote            = VoteRule::Anyone;
	each[11].rampageVoteS           = 5;
	each[12].abandonedCarS          = 600;
	bool allBack = true;
	for (const ServerConfig &one : each) {
		ServerConfig read;
		read.Parse(one.ToIni());
		allBack = allBack && read == one && one != ServerConfig{};
	}
	Check(allBack, "and each of them, changed alone, reads back as itself");
}

// The two things the server does outside the session when it starts: look up
// the public address and ask the router for the port. On unless the host says
// otherwise, since a host who has to learn about port forwarding first is a
// host whose friends never get in.
void TestTheRouterAndThePublicAddress() {
	std::printf("the public address and the router\n");

	const ServerConfig c;
	Check(c.lookUpPublicAddress, "the public address is looked up by default");
	Check(c.openRouterPort, "and the router asked for the port");

	ServerConfig p;
	Check(p.Parse("lookUpPublicAddress = false\nopenRouterPort = no\n") &&
	          !p.lookUpPublicAddress && !p.openRouterPort,
	      "both can be turned off");
	p.Parse("LOOKUPPUBLICADDRESS = on\nupnp = 1\n");
	Check(p.lookUpPublicAddress && p.openRouterPort,
	      "in any case, and upnp is taken for the router's");

	const std::string ini = c.ToIni();
	Check(ini.find("lookUpPublicAddress = true\n") != std::string::npos &&
	          ini.find("openRouterPort = true\n") != std::string::npos,
	      "the file says both, under the names it reads");

	ServerConfig each[2];
	each[0].lookUpPublicAddress = false;
	each[1].openRouterPort      = false;
	bool allBack = true;
	for (const ServerConfig &one : each) {
		ServerConfig read;
		read.Parse(one.ToIni());
		allBack = allBack && read == one && one != ServerConfig{};
	}
	Check(allBack, "and each, changed alone, reads back as itself and is a different config");
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

// The campaign kept between sittings (server/core/progress.h). On unless the
// host says otherwise: a group playing over several evenings shouldn't lose
// its missions because the server was closed overnight.
void TestKeepProgress() {
	std::printf("the progress kept between sittings\n");
	const ServerConfig c;
	Check(c.keepProgress, "kept by default");
	ServerConfig off;
	Check(off.Parse("keepProgress = false\n") && !off.keepProgress, "keepProgress = false turns it off");
	off.Parse("KEEPPROGRESS = yes\n");
	Check(off.keepProgress, "and the key is read in any case");
	Check(c.ToIni().find("keepProgress = true") != std::string::npos, "the file says so");
	ServerConfig changed;
	changed.keepProgress = false;
	ServerConfig read;
	read.Parse(changed.ToIni());
	Check(read == changed && changed != ServerConfig{}, "and it reads back as itself");
}

// The players' custom skins (docs/protocol.md 1.72). On by default: the skin
// somebody picked is theirs to show.
void TestCustomSkins() {
	std::printf("custom skins\n");
	const ServerConfig c;
	Check(c.syncCustomSkins, "sent by default");
	Check(c.ToIni().find("syncCustomSkins = true") != std::string::npos, "the file says so");
	ServerConfig off;
	Check(off.Parse("syncCustomSkins = false\n") && !off.syncCustomSkins,
	      "syncCustomSkins = false turns it off");
	off.Parse("SYNCCUSTOMSKINS = on\n");
	Check(off.syncCustomSkins, "and the key is read in any case");
	ServerConfig changed;
	changed.syncCustomSkins = false;
	ServerConfig read;
	read.Parse(changed.ToIni());
	Check(read == changed && changed != ServerConfig{}, "and it reads back as itself");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	TestDefaults();
	TestParse();
	TestRoundTrip();
	TestNames();
	TestTheSettingsThatWereBuiltIn();
	TestCheats();
	TestCoopCheats();
	TestMoney();
	TestHiddenPackages();
	TestPassword();
	TestTheRouterAndThePublicAddress();
	TestKeepProgress();
	TestCustomSkins();

	std::printf("\n%s\n", g_failures == 0 ? "all server config checks passed"
	                                      : "server config checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
