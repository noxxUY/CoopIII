#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

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

bool IEquals(const std::string &a, const char *b) {
	size_t i = 0;
	for (; i < a.size() && b[i]; ++i) {
		const unsigned char ca = static_cast<unsigned char>(a[i]);
		const unsigned char cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb))
			return false;
	}
	return i == a.size() && b[i] == '\0';
}

bool ParseBool(const std::string &v, bool fallback) {
	if (IEquals(v, "1") || IEquals(v, "true") || IEquals(v, "yes") || IEquals(v, "on"))
		return true;
	if (IEquals(v, "0") || IEquals(v, "false") || IEquals(v, "no") || IEquals(v, "off"))
		return false;
	return fallback;
}

} // namespace

std::string Config::SanitizeNick(const std::string &raw) {
	std::string out;
	out.reserve(raw.size());
	for (char c : Trim(raw)) {
		// Control chars would break chat/name rendering, and this nick comes
		// from a file the player edits by hand, so don't trust it.
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
			continue;
		out += c;
		if (out.size() >= NICK_LEN - 1)
			break;
	}
	return out.empty() ? std::string("Player") : out;
}

bool Config::ParseIni(const std::string &text) {
	if (text.empty())
		return false;

	size_t pos = 0;
	while (pos <= text.size()) {
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos)
			eol = text.size();

		const std::string line = Trim(text.substr(pos, eol - pos));
		pos = eol + 1;

		if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		const std::string key   = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));

		if (IEquals(key, "host") || IEquals(key, "server") || IEquals(key, "ip")) {
			if (!value.empty())
				host = value;
		} else if (IEquals(key, "port")) {
			const long p = std::strtol(value.c_str(), nullptr, 10);
			if (p > 0 && p <= 65535)
				port = static_cast<uint16_t>(p);
		} else if (IEquals(key, "nick") || IEquals(key, "name")) {
			nick = SanitizeNick(value);
		} else if (IEquals(key, "logtofile") || IEquals(key, "log")) {
			logToFile = ParseBool(value, logToFile);
		} else if (IEquals(key, "menupausesthegame") || IEquals(key, "pause")) {
			menuPausesTheGame = ParseBool(value, menuPausesTheGame);
		} else if (IEquals(key, "nametagscale")) {
			const double s = std::strtod(value.c_str(), nullptr);
			if (s >= 0.25 && s <= 4.0)
				nametagScale = static_cast<float>(s);
		} else if (IEquals(key, "seatkey")) {
			// One letter or digit, taken as its virtual-key code, which for
			// A-Z and 0-9 is the uppercase character itself. Anything longer is
			// a name we have no table for, so it is left alone rather than
			// guessed at.
			if (value.size() == 1) {
				const char c = value[0];
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				    (c >= '0' && c <= '9'))
					seatKey = std::toupper(static_cast<unsigned char>(c));
			}
		}
		// Unknown keys: ignored on purpose (see config.h).

		if (eol == text.size())
			break;
	}
	return true;
}

bool Config::LoadFromFile(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return false;

	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	if (size <= 0) {
		std::fclose(fh);
		return false;
	}

	std::string text(static_cast<size_t>(size), '\0');
	const size_t got = std::fread(&text[0], 1, text.size(), fh);
	std::fclose(fh);
	text.resize(got);

	return ParseIni(text);
}

void Config::ApplyOverrides(const char *hostEnv, const char *portEnv, const char *nickEnv) {
	if (hostEnv && hostEnv[0] != '\0')
		host = Trim(hostEnv);
	if (portEnv && portEnv[0] != '\0') {
		const long p = std::strtol(portEnv, nullptr, 10);
		if (p > 0 && p <= 65535)
			port = static_cast<uint16_t>(p);
	}
	if (nickEnv && nickEnv[0] != '\0')
		nick = SanitizeNick(nickEnv);
}

void Config::ApplyEnvOverrides() {
	ApplyOverrides(std::getenv("COOPIII_HOST"), std::getenv("COOPIII_PORT"),
	               std::getenv("COOPIII_NICK"));
}

std::string Config::IniPath() {
	if (const char *env = std::getenv("COOPIII_INI"))
		if (env[0] != '\0')
			return std::string(env);
	return PathNextToModule("CoopIII.ini");
}

std::string Config::PathNextToModule(const char *filename) {
#ifdef _WIN32
	HMODULE self = nullptr;
	// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS resolves the module owning
	// this code, not the process itself. The .asi can sit in the game root or under
	// modloader/ (docs/compat.md §3), and config goes wherever it is.
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(&Config::PathNextToModule), &self) &&
	    self) {
		char buf[MAX_PATH] = {0};
		const DWORD len = GetModuleFileNameA(self, buf, MAX_PATH);
		if (len > 0 && len < MAX_PATH) {
			std::string dir(buf, len);
			const size_t slash = dir.find_last_of("\\/");
			if (slash != std::string::npos)
				return dir.substr(0, slash + 1) + filename;
		}
	}
#endif
	return std::string(filename);
}

} // namespace coopiii
