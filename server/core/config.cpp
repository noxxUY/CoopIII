#include "config.h"

#include <coopiii/mission.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

namespace coopiii {
namespace {

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

bool TruthY(const std::string &text) {
	return _stricmp(text.c_str(), "true") == 0 || _stricmp(text.c_str(), "yes") == 0 ||
	       _stricmp(text.c_str(), "on") == 0 || text == "1";
}

} // namespace

const char *Name(WantedLevelRule rule) {
	switch (rule) {
	case WantedLevelRule::PerPlayer: return "perplayer";
	case WantedLevelRule::Shared:    return "shared";
	case WantedLevelRule::Off:       return "off";
	}
	return "perplayer";
}

const char *Label(WantedLevelRule rule) {
	switch (rule) {
	case WantedLevelRule::PerPlayer: return "Per player";
	case WantedLevelRule::Shared:    return "Shared";
	case WantedLevelRule::Off:       return "Off";
	}
	return "Per player";
}

bool ParseWantedLevel(const std::string &text, WantedLevelRule *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "perplayer") == 0 || _stricmp(t.c_str(), "per-player") == 0) {
		*out = WantedLevelRule::PerPlayer;
		return true;
	}
	if (_stricmp(t.c_str(), "shared") == 0) {
		*out = WantedLevelRule::Shared;
		return true;
	}
	if (_stricmp(t.c_str(), "off") == 0 || _stricmp(t.c_str(), "none") == 0) {
		*out = WantedLevelRule::Off;
		return true;
	}
	return false;
}

const char *Name(RampageMode rule) {
	switch (rule) {
	case RampageMode::Shared: return "shared";
	case RampageMode::Scaled: return "scaled";
	case RampageMode::Off:    return "off";
	}
	return "shared";
}

const char *Label(RampageMode rule) {
	switch (rule) {
	case RampageMode::Shared: return "Shared";
	case RampageMode::Scaled: return "Scaled to players";
	case RampageMode::Off:    return "Off";
	}
	return "Shared";
}

bool ParseRampage(const std::string &text, RampageMode *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "shared") == 0) {
		*out = RampageMode::Shared;
		return true;
	}
	if (_stricmp(t.c_str(), "scaled") == 0 || _stricmp(t.c_str(), "scale") == 0) {
		*out = RampageMode::Scaled;
		return true;
	}
	if (_stricmp(t.c_str(), "off") == 0 || _stricmp(t.c_str(), "none") == 0 ||
	    _stricmp(t.c_str(), "perplayer") == 0) {
		*out = RampageMode::Off;
		return true;
	}
	return false;
}

const char *Name(CheatMode rule) {
	switch (rule) {
	case CheatMode::Shared:   return "shared";
	case CheatMode::Personal: return "personal";
	case CheatMode::Off:      return "off";
	}
	return "shared";
}

const char *Label(CheatMode rule) {
	switch (rule) {
	case CheatMode::Shared:   return "Shared";
	case CheatMode::Personal: return "Personal only";
	case CheatMode::Off:      return "Off";
	}
	return "Shared";
}

bool ParseCheats(const std::string &text, CheatMode *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "shared") == 0 || _stricmp(t.c_str(), "on") == 0 ||
	    _stricmp(t.c_str(), "all") == 0) {
		*out = CheatMode::Shared;
		return true;
	}
	if (_stricmp(t.c_str(), "personal") == 0 || _stricmp(t.c_str(), "self") == 0) {
		*out = CheatMode::Personal;
		return true;
	}
	if (_stricmp(t.c_str(), "off") == 0 || _stricmp(t.c_str(), "none") == 0) {
		*out = CheatMode::Off;
		return true;
	}
	return false;
}

const char *Name(CoopCheatMode rule) {
	switch (rule) {
	case CoopCheatMode::OutsideMissions: return "outsidemissions";
	case CoopCheatMode::Always:          return "always";
	case CoopCheatMode::Off:             return "off";
	}
	return "outsidemissions";
}

const char *Label(CoopCheatMode rule) {
	switch (rule) {
	case CoopCheatMode::OutsideMissions: return "Outside missions";
	case CoopCheatMode::Always:          return "Always";
	case CoopCheatMode::Off:             return "Off";
	}
	return "Outside missions";
}

bool ParseCoopCheats(const std::string &text, CoopCheatMode *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "outsidemissions") == 0 || _stricmp(t.c_str(), "on") == 0 ||
	    _stricmp(t.c_str(), "true") == 0) {
		*out = CoopCheatMode::OutsideMissions;
		return true;
	}
	if (_stricmp(t.c_str(), "always") == 0 || _stricmp(t.c_str(), "missions") == 0) {
		*out = CoopCheatMode::Always;
		return true;
	}
	if (_stricmp(t.c_str(), "off") == 0 || _stricmp(t.c_str(), "false") == 0 ||
	    _stricmp(t.c_str(), "none") == 0) {
		*out = CoopCheatMode::Off;
		return true;
	}
	return false;
}

const char *Name(MoneyMode rule) {
	switch (rule) {
	case MoneyMode::Off:    return "off";
	case MoneyMode::Own:    return "own";
	case MoneyMode::Shared: return "shared";
	}
	return "off";
}

const char *Label(MoneyMode rule) {
	switch (rule) {
	case MoneyMode::Off:    return "Off";
	case MoneyMode::Own:    return "Own wallets";
	case MoneyMode::Shared: return "Shared";
	}
	return "Off";
}

bool ParseMoney(const std::string &text, MoneyMode *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "off") == 0 || _stricmp(t.c_str(), "none") == 0) {
		*out = MoneyMode::Off;
		return true;
	}
	if (_stricmp(t.c_str(), "own") == 0 || _stricmp(t.c_str(), "perplayer") == 0) {
		*out = MoneyMode::Own;
		return true;
	}
	if (_stricmp(t.c_str(), "shared") == 0 || _stricmp(t.c_str(), "pooled") == 0) {
		*out = MoneyMode::Shared;
		return true;
	}
	return false;
}

const char *Name(PackageMode rule) {
	return rule == PackageMode::PerPlayer ? "perplayer" : "shared";
}

const char *Label(PackageMode rule) {
	return rule == PackageMode::PerPlayer ? "Per player" : "Shared";
}

bool ParsePackages(const std::string &text, PackageMode *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "shared") == 0) {
		*out = PackageMode::Shared;
		return true;
	}
	if (_stricmp(t.c_str(), "perplayer") == 0 || _stricmp(t.c_str(), "per-player") == 0 ||
	    _stricmp(t.c_str(), "own") == 0) {
		*out = PackageMode::PerPlayer;
		return true;
	}
	return false;
}

const char *Name(VoteRule rule) {
	switch (rule) {
	case VoteRule::Most:   return "most";
	case VoteRule::Half:   return "half";
	case VoteRule::All:    return "all";
	case VoteRule::Anyone: return "anyone";
	}
	return "most";
}

const char *Label(VoteRule rule) {
	switch (rule) {
	case VoteRule::Most:   return "75%";
	case VoteRule::Half:   return "Half";
	case VoteRule::All:    return "Everyone";
	case VoteRule::Anyone: return "Anyone";
	}
	return "75%";
}

bool ParseVoteRule(const std::string &text, VoteRule *out) {
	const std::string t = Trim(text);
	if (_stricmp(t.c_str(), "most") == 0 || t == "75" || t == "75%") {
		*out = VoteRule::Most;
		return true;
	}
	if (_stricmp(t.c_str(), "half") == 0 || t == "50" || t == "50%") {
		*out = VoteRule::Half;
		return true;
	}
	if (_stricmp(t.c_str(), "all") == 0 || _stricmp(t.c_str(), "everyone") == 0) {
		*out = VoteRule::All;
		return true;
	}
	if (_stricmp(t.c_str(), "anyone") == 0 || _stricmp(t.c_str(), "one") == 0) {
		*out = VoteRule::Anyone;
		return true;
	}
	return false;
}

const char *MissionEnemiesName(uint8_t rule) {
	return rule == MISSION_ENEMIES_MORE      ? "more"
	       : rule == MISSION_ENEMIES_TOUGHER ? "tougher"
	                                         : "original";
}

const char *MissionEnemiesLabel(uint8_t rule) {
	return rule == MISSION_ENEMIES_MORE      ? "More"
	       : rule == MISSION_ENEMIES_TOUGHER ? "Tougher"
	                                         : "Original";
}

namespace {

// A whole number in [lo, hi], or false and `out` left alone.
template <class T>
bool ParseWhole(const std::string &value, long lo, long hi, T *out) {
	char       *end = nullptr;
	const long  n   = std::strtol(value.c_str(), &end, 10);
	if (end == value.c_str() || n < lo || n > hi)
		return false;
	*out = static_cast<T>(n);
	return true;
}

} // namespace

std::string ServerConfig::Path() {
	char        buf[MAX_PATH] = {0};
	const DWORD n             = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return (slash == std::string::npos ? std::string() : s.substr(0, slash + 1)) +
	       "CoopIII-Server.ini";
}

bool ServerConfig::Parse(const std::string &text) {
	if (text.empty())
		return false;

	size_t pos = 0;
	while (pos < text.size()) {
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos)
			eol = text.size();
		const std::string line = Trim(text.substr(pos, eol - pos));
		pos                    = eol + 1;

		if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		const std::string key   = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));

		if (_stricmp(key.c_str(), "port") == 0) {
			const int parsed = std::atoi(value.c_str());
			if (parsed > 0 && parsed <= 65535)
				port = static_cast<uint16_t>(parsed);
		} else if (_stricmp(key.c_str(), "friendlyfire") == 0) {
			friendlyFire = TruthY(value);
		} else if (_stricmp(key.c_str(), "wantedlevel") == 0) {
			WantedLevelRule rule = wantedLevel;
			if (ParseWantedLevel(value, &rule))
				wantedLevel = rule;
		} else if (_stricmp(key.c_str(), "missionfailondeath") == 0) {
			missionFailOnDeath = TruthY(value);
		} else if (_stricmp(key.c_str(), "missionmargin") == 0) {
			// Metres, a fraction allowed, up to 50: past that "there" means
			// the whole neighbourhood and the rule means nothing.
			char        *end = nullptr;
			const double m   = std::strtod(value.c_str(), &end);
			if (end != value.c_str() && m >= 0.0 && m <= 50.0)
				missionMarginCm = static_cast<uint16_t>(m * 100.0 + 0.5);
		} else if (_stricmp(key.c_str(), "missionenemies") == 0) {
			const std::string t = Trim(value);
			if (_stricmp(t.c_str(), "original") == 0)
				missionEnemies = MISSION_ENEMIES_ORIGINAL;
			else if (_stricmp(t.c_str(), "tougher") == 0)
				missionEnemies = MISSION_ENEMIES_TOUGHER;
			else if (_stricmp(t.c_str(), "more") == 0)
				missionEnemies = MISSION_ENEMIES_MORE;
		} else if (_stricmp(key.c_str(), "missionscale") == 0) {
			char       *end = nullptr;
			const long  pct = std::strtol(value.c_str(), &end, 10);
			if (end != value.c_str() && pct >= 0 && pct <= MISSION_SCALE_MAX)
				missionScale = static_cast<uint16_t>(pct);
		} else if (_stricmp(key.c_str(), "ammosync") == 0) {
			ammoSync = TruthY(value);
		} else if (_stricmp(key.c_str(), "synccustomskins") == 0) {
			syncCustomSkins = TruthY(value);
		} else if (_stricmp(key.c_str(), "rampages") == 0) {
			RampageMode rule = rampage;
			if (ParseRampage(value, &rule))
				rampage = rule;
		} else if (_stricmp(key.c_str(), "cheats") == 0) {
			CheatMode rule = cheats;
			if (ParseCheats(value, &rule))
				cheats = rule;
		} else if (_stricmp(key.c_str(), "coopcheats") == 0) {
			CoopCheatMode rule = coopCheats;
			if (ParseCoopCheats(value, &rule))
				coopCheats = rule;
		} else if (_stricmp(key.c_str(), "money") == 0) {
			MoneyMode rule = money;
			if (ParseMoney(value, &rule))
				money = rule;
		} else if (_stricmp(key.c_str(), "hiddenpackages") == 0) {
			PackageMode rule = hiddenPackages;
			if (ParsePackages(value, &rule))
				hiddenPackages = rule;
		} else if (_stricmp(key.c_str(), "keepprogress") == 0) {
			keepProgress = TruthY(value);
		} else if (_stricmp(key.c_str(), "password") == 0) {
			password = CleanPassword(value);
		} else if (_stricmp(key.c_str(), "maxplayers") == 0) {
			ParseWhole(value, 1, MAX_PLAYERS, &maxPlayers);
		} else if (_stricmp(key.c_str(), "maxwantedlevel") == 0) {
			ParseWhole(value, CONFIG_MAX_WANTED_MIN, WANTED_LEVEL_CEILING, &maxWanted);
		} else if (_stricmp(key.c_str(), "missioncheckpointwait") == 0) {
			ParseWhole(value, 0, MISSION_CHECKPOINT_WAIT_S_MAX, &missionCheckpointWaitS);
		} else if (_stricmp(key.c_str(), "missiontimedcheckpoints") == 0) {
			missionTimedCheckpoints = TruthY(value);
		} else if (_stricmp(key.c_str(), "missioncatchup") == 0) {
			ParseWhole(value, 0, MISSION_DISTANCE_M_MAX, &missionCatchUpM);
		} else if (_stricmp(key.c_str(), "missionfallbehind") == 0) {
			ParseWhole(value, 0, MISSION_DISTANCE_M_MAX, &missionBehindM);
		} else if (_stricmp(key.c_str(), "missionfallbehindtime") == 0) {
			ParseWhole(value, CONFIG_BEHIND_S_MIN, MISSION_BEHIND_S_MAX, &missionBehindS);
		} else if (_stricmp(key.c_str(), "missionintrowait") == 0) {
			ParseWhole(value, CONFIG_INTRO_WAIT_S_MIN, CONFIG_INTRO_WAIT_S_MAX,
			           &missionIntroWaitS);
		} else if (_stricmp(key.c_str(), "missionpayhelpers") == 0) {
			missionPayHelpers = TruthY(value);
		} else if (_stricmp(key.c_str(), "cutsceneskip") == 0) {
			VoteRule rule = cutsceneSkip;
			if (ParseVoteRule(value, &rule))
				cutsceneSkip = rule;
		} else if (_stricmp(key.c_str(), "rampagevote") == 0) {
			VoteRule rule = rampageVote;
			if (ParseVoteRule(value, &rule))
				rampageVote = rule;
		} else if (_stricmp(key.c_str(), "rampagevotetime") == 0) {
			ParseWhole(value, RAMPAGE_VOTE_MS_MIN / 1000, RAMPAGE_VOTE_MS_MAX / 1000,
			           &rampageVoteS);
		} else if (_stricmp(key.c_str(), "abandonedcars") == 0) {
			ParseWhole(value, CONFIG_ABANDONED_S_MIN, CONFIG_ABANDONED_S_MAX, &abandonedCarS);
		} else if (_stricmp(key.c_str(), "lookuppublicaddress") == 0) {
			lookUpPublicAddress = TruthY(value);
		} else if (_stricmp(key.c_str(), "openrouterport") == 0 ||
		           _stricmp(key.c_str(), "upnp") == 0) {
			openRouterPort = TruthY(value);
		}
	}
	return true;
}

std::string ServerConfig::CleanPassword(const std::string &text) {
	std::string out;
	for (const char c : text) {
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
			continue;
		if (out.size() + 1 >= PASSWORD_LEN)
			break;
		out.push_back(c);
	}
	return out;
}

bool ServerConfig::Load(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return false;

	std::string text;
	char        buf[4096];
	size_t      n;
	while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
		text.append(buf, n);
	std::fclose(fh);
	return Parse(text);
}

namespace {

void Appendf(std::string &out, const char *fmt, ...) {
	char    buf[1024];
	va_list args;
	va_start(args, fmt);
	const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n > 0)
		out.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
		                                                     : sizeof(buf) - 1);
}

const char *YesNo(bool on) { return on ? "true" : "false"; }

} // namespace

std::string ServerConfig::ToIni() const {
	std::string out;
	out.reserve(12000);

	out += "; CoopIII server. Sits next to server.exe; both the console server and\n"
	       "; the window read it, and the options dialog writes it. A key this build\n"
	       "; does not know is ignored, and so is a value that makes no sense.\n"
	       ";\n"
	       "; The defaults are docs/roadmap.md 5: where single player and co-op\n"
	       "; convenience disagree, single player wins.\n"
	       "\n"
	       "[CoopIII]\n";

	// ---- server ----
	out += "\n"
	       "; ---- Server ----\n"
	       "\n"
	       "; UDP port to listen on. Players have to use the same one. Players who\n"
	       "; are not on this machine's network usually reach it through the\n"
	       "; router, which then has to forward this UDP port to this machine -\n"
	       "; not with a public address of its own, or a VPN everybody is on. The\n"
	       "; server's log says which applies when it starts. Takes a restart.\n";
	Appendf(out, "port = %u\n", static_cast<unsigned>(port));
	out += "\n"
	       "; What players have to give to join, or nothing for anybody who knows\n"
	       "; the address. Each of them puts the same in their CoopIII.ini as\n"
	       "; `password = ...`. It crosses the network as it is typed, so it keeps\n"
	       "; strangers out and nothing more; up to 31 characters.\n";
	Appendf(out, "password = %s\n", password.c_str());
	out += "\n"
	       "; How many players the session takes, 1 to 8. Lowering it turns\n"
	       "; nobody out; it only stops the next one joining.\n";
	Appendf(out, "maxPlayers = %u\n", static_cast<unsigned>(maxPlayers));
	out += "\n"
	       "; Whether the server asks a what-is-my-IP service (api.ipify.org, or\n"
	       "; checkip.amazonaws.com or icanhazip.com if that one does not answer)\n"
	       "; for this network's public address when it starts, so it can show the\n"
	       "; address friends on the internet connect to. Takes a restart.\n";
	Appendf(out, "lookUpPublicAddress = %s\n", YesNo(lookUpPublicAddress));
	out += "\n"
	       "; Whether the server asks the router, over UPnP, to forward the port to\n"
	       "; this machine when it starts, and to stop again when it stops. A router\n"
	       "; with UPnP turned off says no, and then the port has to be forwarded by\n"
	       "; hand. Takes a restart.\n";
	Appendf(out, "openRouterPort = %s\n", YesNo(openRouterPort));

	// ---- players and combat ----
	out += "\n"
	       "; ---- Players and combat ----\n"
	       "\n"
	       "; Whether players can hurt and kill each other. Off by default.\n";
	Appendf(out, "friendlyFire = %s\n", YesNo(friendlyFire));
	out += "\n"
	       "; Whether everybody sees everybody else's real ammunition. Off by\n"
	       "; default, and with it off a remote player's gun never runs dry on\n"
	       "; your screen. It does not share weapons - players still carry\n"
	       "; whatever they picked up, this only makes the counts honest.\n";
	Appendf(out, "ammoSync = %s\n", YesNo(ammoSync));
	out += "\n"
	       "; Whether everybody sees everybody else's custom skin from Player Setup.\n";
	Appendf(out, "syncCustomSkins = %s\n", YesNo(syncCustomSkins));

	// ---- wanted level ----
	out += "\n"
	       "; ---- Wanted level ----\n"
	       "\n"
	       "; How the police treat the session:\n"
	       ";   perplayer  everyone keeps their own stars, shared while riding\n"
	       ";              in the same car (the default)\n"
	       ";   shared     the whole session shares the highest wanted level\n"
	       ";   off        no wanted level at all\n";
	Appendf(out, "wantedLevel = %s\n", Name(wantedLevel));
	out += "\n"
	       "; The most stars anybody can have, 1 to 6. 6 is the game's own.\n";
	Appendf(out, "maxWantedLevel = %u\n", static_cast<unsigned>(maxWanted));

	// ---- missions ----
	out += "\n"
	       "; ---- Missions ----\n"
	       "; For the missions the session shares, which the players' games do\n"
	       "; with `missions = on` in their CoopIII.ini.\n"
	       "\n"
	       "; If anyone dies or is busted during a mission, it fails for everyone,\n"
	       "; as it does in single player.\n";
	Appendf(out, "missionFailOnDeath = %s\n", YesNo(missionFailOnDeath));
	out += "\n"
	       "; How far outside one of a mission's checkpoints still counts as\n"
	       "; being there, in metres, up to 50. Everybody has to be there before\n"
	       "; a mission moves on; 5 lets a friend parked beside you count. Its\n"
	       "; start counts anybody on foot within 5 m, or within this when wider.\n";
	Appendf(out, "missionMargin = %g\n", MarginMetres(missionMarginCm));
	out += "\n"
	       "; How long a checkpoint waits for the players who are not at it before\n"
	       "; the mission goes on without them, in seconds, up to 600. 0 and no\n"
	       "; checkpoint waits for anybody.\n";
	Appendf(out, "missionCheckpointWait = %u\n", static_cast<unsigned>(missionCheckpointWaitS));
	out += "\n"
	       "; Whether checkpoints wait in races, side jobs and missions with a\n"
	       "; clock on the screen too. Off by default, because the clock and the\n"
	       "; rivals do not wait; turned on, the clock keeps running.\n";
	Appendf(out, "missionTimedCheckpoints = %s\n", YesNo(missionTimedCheckpoints));
	out += "\n"
	       "; A player who joins in the middle of a mission, or comes back from the\n"
	       "; hospital or the police station, is brought beside its owner when\n"
	       "; further away than this, in metres. 0 and nobody is.\n";
	Appendf(out, "missionCatchUp = %u\n", static_cast<unsigned>(missionCatchUpM));
	out += "\n"
	       "; Where checkpoints do not wait, a player further than this from the\n"
	       "; owner, in metres, for this long, in seconds, is brought beside them.\n"
	       "; 0 metres and nobody is.\n";
	Appendf(out, "missionFallBehind = %u\n", static_cast<unsigned>(missionBehindM));
	Appendf(out, "missionFallBehindTime = %u\n", static_cast<unsigned>(missionBehindS));
	out += "\n"
	       "; How long a mission's start waits for a player whose game is busy\n"
	       "; with a mission of its own, a new game's intro say, before it goes\n"
	       "; on without them, in seconds, 10 to 600.\n";
	Appendf(out, "missionIntroWait = %u\n", static_cast<unsigned>(missionIntroWaitS));
	out += "\n"
	       "; How a shared mission's enemies stand up to more than one player:\n"
	       ";   original   as in single player\n"
	       ";   tougher    their health and armour grow with the players\n"
	       ";   more       tougher, and more of them\n"
	       "; and what each player after the first adds to them, in percent.\n";
	Appendf(out, "missionEnemies = %s\n", MissionEnemiesName(missionEnemies));
	Appendf(out, "missionScale = %u\n", static_cast<unsigned>(missionScale));
	out += "\n"
	       "; Whether everybody in a mission gets its reward, or only the player\n"
	       "; who started it. With money = shared there is one wallet, and it is\n"
	       "; paid once either way.\n";
	Appendf(out, "missionPayHelpers = %s\n", YesNo(missionPayHelpers));
	out += "\n"
	       "; How many of the players watching a cutscene have to press skip:\n"
	       ";   most     75% of them, rounded up (the default)\n"
	       ";   half     half of them, rounded up\n"
	       ";   all      every one of them\n"
	       ";   anyone   the first one to press it\n";
	Appendf(out, "cutsceneSkip = %s\n", Name(cutsceneSkip));

	// ---- money and progress ----
	out += "\n"
	       "; ---- Money and progress ----\n"
	       "\n"
	       "; What happens to the players' cash:\n"
	       ";   off     every machine pays its own player for what its own game\n"
	       ";           saw, as it always has (the default)\n"
	       ";   own     everyone keeps their own money, but the reward for a car\n"
	       ";           or a police helicopter goes to whoever destroyed it, once\n"
	       ";   shared  one wallet for everybody: anything anyone earns, spends\n"
	       ";           or is fined comes out of the same money\n";
	Appendf(out, "money = %s\n", Name(money));
	out += "\n"
	       "; Who a hidden package counts for:\n"
	       ";   shared     one player collects it and it is gone for everybody,\n"
	       ";              and everybody's count goes up (the default)\n"
	       ";   perplayer  every player finds their own hundred\n";
	Appendf(out, "hiddenPackages = %s\n", Name(hiddenPackages));
	out += "\n"
	       "; Whether the missions passed, the hidden packages found and the\n"
	       "; Import/Export and crane lists are kept in CoopIII-Progress.dat next\n"
	       "; to server.exe, so the next time the server starts the session picks\n"
	       "; up where it was and a player who is behind catches up when he\n"
	       "; joins. Turned off, the file is left alone. To start the campaign\n"
	       "; over, stop the server and delete the file, or use the options\n"
	       "; window's Forget progress, or start it with -forgetprogress. Takes a\n"
	       "; restart.\n";
	Appendf(out, "keepProgress = %s\n", YesNo(keepProgress));
	out += "\n"
	       "; What a rampage is worth in a group:\n"
	       ";   shared   one rampage for the whole session, everybody's kills\n"
	       ";            count toward it, and the target is the one the game\n"
	       ";            asks for (the default)\n"
	       ";   scaled   the same, but the target is multiplied by the number of\n"
	       ";            players - four of you murder 80 Diablos, not 20\n"
	       ";   off      nobody's kills are shared; each machine counts only its\n"
	       ";            own player and can end the rampage differently\n"
	       "; A change while a rampage runs waits for it to end.\n";
	Appendf(out, "rampages = %s\n", Name(rampage));
	out += "\n"
	       "; How many players have to say yes before a rampage one of them\n"
	       "; picked up starts for everybody - most, half, all or anyone, as for\n"
	       "; cutsceneSkip - and how long they have to answer, 5 to 60 seconds.\n";
	Appendf(out, "rampageVote = %s\n", Name(rampageVote));
	Appendf(out, "rampageVoteTime = %u\n", static_cast<unsigned>(rampageVoteS));

	// ---- world ----
	out += "\n"
	       "; ---- World ----\n"
	       "\n"
	       "; What a cheat typed by one player does to everybody else:\n"
	       ";   shared    every cheat works. The ones about the player who typed\n"
	       ";             them stay theirs; a weather cheat changes the host's sky,\n"
	       ";             which is everybody's, and the game speed, MADWEATHER,\n"
	       ";             ITSALLGOINGMAAAD and WEAPONSFORALL happen for everybody\n"
	       ";             (the default)\n"
	       ";   personal  only the cheats about the player who typed them: health,\n"
	       ";             armour, weapons, money, stars, skins, the tank, the car\n"
	       ";             handling ones\n"
	       ";   off       no cheats at all while connected\n";
	Appendf(out, "cheats = %s\n", Name(cheats));
	out += "\n"
	       "; CoopIII's own cheats, typed the same way: TPTO and a player's number\n"
	       "; from the Tab list (TPTO1 to TPTO8) puts you beside that player, in\n"
	       "; your car if you are driving one.\n"
	       ";   outsidemissions  they work, but not during a mission (the default)\n"
	       ";   always           they work during a mission too\n"
	       ";   off              they never work\n";
	Appendf(out, "coopCheats = %s\n", Name(coopCheats));
	out += "\n"
	       "; How long a car the players have used stays once nobody is in it or\n"
	       "; within 200 m of it, in seconds, 10 to 600.\n";
	Appendf(out, "abandonedCars = %u\n", static_cast<unsigned>(abandonedCarS));
	return out;
}

bool ServerConfig::Save(const std::string &path) const {
	const std::string text = ToIni();
	FILE             *fh   = std::fopen(path.c_str(), "wb");
	if (!fh)
		return false;
	const size_t written = std::fwrite(text.data(), 1, text.size(), fh);
	std::fclose(fh);
	return written == text.size();
}

} // namespace coopiii
