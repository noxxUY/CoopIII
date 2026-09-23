#include "config.h"

#include <cstdio>
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
		} else if (_stricmp(key.c_str(), "ammosync") == 0) {
			ammoSync = TruthY(value);
		} else if (_stricmp(key.c_str(), "rampages") == 0) {
			RampageMode rule = rampage;
			if (ParseRampage(value, &rule))
				rampage = rule;
		} else if (_stricmp(key.c_str(), "cheats") == 0) {
			CheatMode rule = cheats;
			if (ParseCheats(value, &rule))
				cheats = rule;
		} else if (_stricmp(key.c_str(), "money") == 0) {
			MoneyMode rule = money;
			if (ParseMoney(value, &rule))
				money = rule;
		}
	}
	return true;
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

std::string ServerConfig::ToIni() const {
	char out[8192];
	std::snprintf(
	    out, sizeof(out),
	    "; CoopIII server. Sits next to server.exe; both the console server and\n"
	    "; the window read it, and the options dialog writes it.\n"
	    ";\n"
	    "; The defaults are docs/roadmap.md 5: where single player and co-op\n"
	    "; convenience disagree, single player wins.\n"
	    "\n"
	    "[CoopIII]\n"
	    "\n"
	    "; UDP port to listen on. Players have to use the same one.\n"
	    "port = %u\n"
	    "\n"
	    "; Whether players can hurt and kill each other. Off by default.\n"
	    "friendlyFire = %s\n"
	    "\n"
	    "; How the police treat the session:\n"
	    ";   perplayer  everyone keeps their own stars, shared while riding\n"
	    ";              in the same car (the default)\n"
	    ";   shared     the whole session shares the highest wanted level\n"
	    ";   off        no wanted level at all\n"
	    "wantedLevel = %s\n"
	    "\n"
	    "; If anyone dies during a mission, it fails for everyone, as it does\n"
	    "; in single player.\n"
	    "missionFailOnDeath = %s\n"
	    "\n"
	    "; Whether everybody sees everybody else's real ammunition. Off by\n"
	    "; default, and with it off a remote player's gun never runs dry on\n"
	    "; your screen. It does not share weapons - players still carry\n"
	    "; whatever they picked up, this only makes the counts honest.\n"
	    "ammoSync = %s\n"
	    "\n"
	    "; What a rampage is worth in a group:\n"
	    ";   shared   one rampage for the whole session, everybody's kills\n"
	    ";            count toward it, and the target is the one the game\n"
	    ";            asks for (the default)\n"
	    ";   scaled   the same, but the target is multiplied by the number of\n"
	    ";            players - four of you murder 80 Diablos, not 20\n"
	    ";   off      nobody's kills are shared; each machine counts only its\n"
	    ";            own player and can end the rampage differently\n"
	    "rampages = %s\n"
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
	    ";   off       no cheats at all while connected\n"
	    "cheats = %s\n"
	    "\n"
	    "; What happens to the players' cash:\n"
	    ";   off     every machine pays its own player for what its own game\n"
	    ";           saw, as it always has (the default)\n"
	    ";   own     everyone keeps their own money, but the reward for a car\n"
	    ";           or a police helicopter goes to whoever destroyed it, once\n"
	    ";   shared  one wallet for everybody: anything anyone earns, spends\n"
	    ";           or is fined comes out of the same money\n"
	    "money = %s\n",
	    port, friendlyFire ? "true" : "false", Name(wantedLevel),
	    missionFailOnDeath ? "true" : "false", ammoSync ? "true" : "false",
	    Name(rampage), Name(cheats), Name(money));
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
