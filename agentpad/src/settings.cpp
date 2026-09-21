#include "settings.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace agentpad {

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

// Returns false when the value isn't a boolean at all, so an unparseable
// setting leaves the default alone instead of quietly turning "off".
bool ParseBool(const std::string &v, bool *out) {
	if (IEquals(v, "1") || IEquals(v, "true") || IEquals(v, "yes") || IEquals(v, "on")) {
		*out = true;
		return true;
	}
	if (IEquals(v, "0") || IEquals(v, "false") || IEquals(v, "no") || IEquals(v, "off")) {
		*out = false;
		return true;
	}
	return false;
}

bool ParseMulti(const std::string &v, Settings::MultiInstance *out) {
	if (IEquals(v, "auto")) {
		*out = Settings::MultiInstance::Auto;
		return true;
	}
	if (IEquals(v, "on") || IEquals(v, "1") || IEquals(v, "true") || IEquals(v, "yes") ||
	    IEquals(v, "always")) {
		*out = Settings::MultiInstance::On;
		return true;
	}
	if (IEquals(v, "off") || IEquals(v, "0") || IEquals(v, "false") || IEquals(v, "no") ||
	    IEquals(v, "never")) {
		*out = Settings::MultiInstance::Off;
		return true;
	}
	return false;
}

} // namespace

const char *Settings::MultiInstanceName(MultiInstance m) {
	switch (m) {
	case MultiInstance::Off:  return "off";
	case MultiInstance::Auto: return "auto";
	case MultiInstance::On:   return "on";
	}
	return "auto";
}

void Settings::ParseIni(const std::string &text) {
	size_t pos = 0;
	while (pos < text.size()) {
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

		if (IEquals(key, "skipintro") || IEquals(key, "skipmovies")) {
			ParseBool(value, &skipIntro);
		} else if (IEquals(key, "multiinstance")) {
			ParseMulti(value, &multiInstance);
		}
		// Unknown keys: ignored on purpose (see settings.h).
	}
}

void Settings::ApplyOverrides(const char *skipIntroValue, const char *multiInstanceValue) {
	if (skipIntroValue && skipIntroValue[0] != '\0')
		ParseBool(Trim(skipIntroValue), &skipIntro);
	if (multiInstanceValue && multiInstanceValue[0] != '\0')
		ParseMulti(Trim(multiInstanceValue), &multiInstance);
}

void Settings::Load(const std::string &path) {
	if (FILE *fh = std::fopen(path.c_str(), "rb")) {
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		if (size > 0) {
			std::string text(static_cast<size_t>(size), '\0');
			const size_t got = std::fread(&text[0], 1, text.size(), fh);
			text.resize(got);
			ParseIni(text);
		}
		std::fclose(fh);
	}

	// Environment wins over the file - ini is the machine's setting, the
	// environment is this run's. A test harness wanting the intro skipped for
	// one launch shouldn't have to edit a file in the game folder and put it
	// back afterward.
	ApplyOverrides(std::getenv("AGENTPAD_SKIPINTRO"), std::getenv("AGENTPAD_MULTIINSTANCE"));
}

} // namespace agentpad
