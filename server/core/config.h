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

// How a rampage behaves. docs/roadmap.md §5.10 settled the first of these and
// it is the default; §5.10 also stated the price of it out loud - four
// players sharing one 20-kill target in two minutes is trivial - and put the
// fix in M5 on the grounds that it needed the script intercepted. It does
// not: CDarkel::StartFrenzy takes the target as an argument and has two
// callers, both of them script opcodes, so `scaled` is a detour and no script
// work at all.
enum class RampageMode : uint8_t {
	Shared = 0,   // §5.10: one rampage, one count, the script's own target
	Scaled = 1,   // the same, with the target multiplied by the player count
	Off    = 2,   // kills are not shared; every machine counts its own
};

static_assert(static_cast<uint8_t>(RampageMode::Shared) == RAMPAGE_RULE_SHARED, "");
static_assert(static_cast<uint8_t>(RampageMode::Scaled) == RAMPAGE_RULE_SCALED, "");
static_assert(static_cast<uint8_t>(RampageMode::Off)    == RAMPAGE_RULE_OFF, "");

inline uint8_t WireValue(RampageMode rule) { return static_cast<uint8_t>(rule); }

const char *Name(RampageMode rule);                // "shared" / "scaled" / "off"
const char *Label(RampageMode rule);               // "Shared" / "Scaled" / "Off"
bool        ParseRampage(const std::string &text, RampageMode *out);

// What a cheat typed by one player does in a session (docs/cheats.md,
// docs/roadmap.md §5.14). The default is the one single player has - every
// cheat works - with each one running on the machine that owns what it
// changes. The other two exist because a riot or a slowed clock that one
// player types is something the others may reasonably not want.
enum class CheatMode : uint8_t {
	Shared   = 0,   // every cheat; world ones go where the world is owned
	Personal = 1,   // only the ones about the player who typed them
	Off      = 2,   // none
};

static_assert(static_cast<uint8_t>(CheatMode::Shared)   == CHEAT_RULE_SHARED, "");
static_assert(static_cast<uint8_t>(CheatMode::Personal) == CHEAT_RULE_PERSONAL, "");
static_assert(static_cast<uint8_t>(CheatMode::Off)      == CHEAT_RULE_OFF, "");

inline uint8_t WireValue(CheatMode rule) { return static_cast<uint8_t>(rule); }

const char *Name(CheatMode rule);                  // "shared" / "personal" / "off"
const char *Label(CheatMode rule);                 // "Shared" / "Personal only" / "Off"
bool        ParseCheats(const std::string &text, CheatMode *out);

// What happens to the players' cash (protocol.h, MoneyRule). Off by default,
// which is what every build before this did: each machine pays its own player
// for whatever its own engine saw.
enum class MoneyMode : uint8_t {
	Off    = 0,   // nothing travels
	Own    = 1,   // own wallets, but an award goes to whoever earned it
	Shared = 2,   // one wallet for the session, kept by the server
};

static_assert(static_cast<uint8_t>(MoneyMode::Off)    == MONEY_RULE_OFF, "");
static_assert(static_cast<uint8_t>(MoneyMode::Own)    == MONEY_RULE_OWN, "");
static_assert(static_cast<uint8_t>(MoneyMode::Shared) == MONEY_RULE_SHARED, "");

inline uint8_t WireValue(MoneyMode rule) { return static_cast<uint8_t>(rule); }

const char *Name(MoneyMode rule);                  // "off" / "own" / "shared"
const char *Label(MoneyMode rule);                 // "Off" / "Own wallets" / "Shared"
bool        ParseMoney(const std::string &text, MoneyMode *out);

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

	// How a rampage behaves (§5.10). `shared` is the decision and the
	// default; `scaled` multiplies the kill target by the number of players
	// so that a group does not finish a two-minute rampage in twenty
	// seconds; `off` keeps every machine counting only its own player, which
	// is what this build did before rampages were shared.
	RampageMode     rampage            = RampageMode::Shared;

	// What a cheat does in a session. `shared`, the default, lets every cheat
	// work and sends the ones about the world to whoever owns what they
	// change; `personal` keeps only the ones about the player who typed them;
	// `off` refuses all of them while connected.
	CheatMode       cheats             = CheatMode::Shared;

	// What happens to the players' cash. `off`, the default, leaves every
	// machine paying its own player for whatever its engine saw; `own` pays
	// an award to whoever earned it; `shared` is one wallet for everybody.
	MoneyMode       money              = MoneyMode::Off;

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
		       ammoSync == other.ammoSync && rampage == other.rampage &&
		       cheats == other.cheats && money == other.money;
	}
	bool operator!=(const ServerConfig &other) const { return !(*this == other); }
};

} // namespace coopiii
