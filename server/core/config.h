// CoopIII-Server.ini - the server's settings, read by both front ends.
//
// Same shape as CoopIII.ini: a `[CoopIII]` section of `key = value` lines with
// `;` comments, because a player who has edited one already knows how to edit
// the other. It sits next to the executable, and both server.exe and
// both front ends read it; the options dialog writes it.
//
// The defaults are docs/roadmap.md §5, and §5.5 is why they are what they are:
// where single-player behaviour and co-op convenience disagree, single player
// wins, and the defaults do not relax it.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>
#include <string>

namespace coopiii {

enum class WantedLevelRule : uint8_t {
	PerPlayer = 0,   // §5.1 default: own stars, shared inside a shared vehicle
	Shared    = 1,   // the whole session shares the highest wanted level
	Off       = 2,   // no wanted level at all
};

// The same three values as protocol.h's WantedRule, and pinned to it rather
// than assumed equal to it. Two enums that only agree with themselves are
// worth nothing; this is what makes them agree with each other, and it is the
// cheapest possible version of the padtest --contract trick.
static_assert(static_cast<uint8_t>(WantedLevelRule::PerPlayer) == WANTED_RULE_PERPLAYER, "");
static_assert(static_cast<uint8_t>(WantedLevelRule::Shared)    == WANTED_RULE_SHARED, "");
static_assert(static_cast<uint8_t>(WantedLevelRule::Off)       == WANTED_RULE_OFF, "");

// What goes in S_Welcome's flags. A named function rather than a cast at the
// call site, so the static_asserts above are what the conversion rests on.
inline uint8_t WireValue(WantedLevelRule rule) { return static_cast<uint8_t>(rule); }

const char *Name(WantedLevelRule rule);            // "perplayer" / "shared" / "off"
const char *Label(WantedLevelRule rule);           // "Per player" / "Shared" / "Off"
bool        ParseWantedLevel(const std::string &text, WantedLevelRule *out);

struct ServerConfig {
	uint16_t        port               = 2001;
	bool            friendlyFire       = false;                     // §5.2
	WantedLevelRule wantedLevel        = WantedLevelRule::PerPlayer; // §5.1
	bool            missionFailOnDeath = true;                      // §5.4

	// Whether a player's real ammunition is reported to everyone else
	// (docs/protocol.md 1.9.6). Off by default, which is the behaviour
	// CoopIII has always had: a remote player's gun is handed a fixed
	// thousand rounds and nobody ever sees anyone run dry.
	//
	// This is not a shared inventory. Players carry whatever they like; all
	// this decides is whether their own counts are honest on other screens.
	bool            ammoSync           = false;

	// Where the file lives: next to the executable, so a server copied to
	// another folder takes its settings with it.
	static std::string Path();

	// A missing file is not an error - the defaults above are a working
	// server, and demanding config before the thing will start makes for a
	// bad first run.
	bool Load(const std::string &path);
	bool Save(const std::string &path) const;

	// Parses INI text. Unknown keys are ignored rather than fatal, so a file
	// from a newer build still loads.
	bool Parse(const std::string &text);

	// The whole file, comments included. Used by Save and worth having on its
	// own so a test can read what would be written.
	std::string ToIni() const;

	bool operator==(const ServerConfig &other) const {
		return port == other.port && friendlyFire == other.friendlyFire &&
		       wantedLevel == other.wantedLevel &&
		       missionFailOnDeath == other.missionFailOnDeath &&
		       ammoSync == other.ammoSync;
	}
	bool operator!=(const ServerConfig &other) const { return !(*this == other); }
};

} // namespace coopiii
