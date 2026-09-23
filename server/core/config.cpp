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
	char out[2048];
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
	    "ammoSync = %s\n",
	    port, friendlyFire ? "true" : "false", Name(wantedLevel),
	    missionFailOnDeath ? "true" : "false", ammoSync ? "true" : "false");
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
