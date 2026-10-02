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

#include "rampagevote.h"

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

// What CoopIII's own typed cheats - TPTO1 to TPTO8 so far - may do (protocol.h,
// CoopCheatRule; docs/cheats.md 7). They move a player about, so by default
// they work outside missions only; `always` lets them into missions too. Not
// tied to `cheats`: that one is about the game's own 23.
enum class CoopCheatMode : uint8_t {
	OutsideMissions = 0,
	Always          = 1,
	Off             = 2,
};

static_assert(static_cast<uint8_t>(CoopCheatMode::OutsideMissions) == COOP_CHEATS_OUTSIDE_MISSIONS, "");
static_assert(static_cast<uint8_t>(CoopCheatMode::Always)          == COOP_CHEATS_ALWAYS, "");
static_assert(static_cast<uint8_t>(CoopCheatMode::Off)             == COOP_CHEATS_OFF, "");

inline uint8_t WireValue(CoopCheatMode rule) { return static_cast<uint8_t>(rule); }

const char *Name(CoopCheatMode rule);              // "outsidemissions" / "always" / "off"
const char *Label(CoopCheatMode rule);             // "Outside missions" / "Always" / "Off"
bool        ParseCoopCheats(const std::string &text, CoopCheatMode *out);

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

// Who a hidden package counts for (docs/roadmap.md 5.11). `shared` is the
// decision and the default: one player collects it and it is gone for all.
// `perplayer` gives every player their own hundred to find.
enum class PackageMode : uint8_t {
	Shared    = 0,
	PerPlayer = 1,
};

static_assert(static_cast<uint8_t>(PackageMode::Shared)    == PACKAGES_SHARED, "");
static_assert(static_cast<uint8_t>(PackageMode::PerPlayer) == PACKAGES_PERPLAYER, "");

inline uint8_t WireValue(PackageMode rule) { return static_cast<uint8_t>(rule); }

const char *Name(PackageMode rule);                // "shared" / "perplayer"
const char *Label(PackageMode rule);               // "Shared" / "Per player"
bool        ParsePackages(const std::string &text, PackageMode *out);

// How many have to say yes to a vote (rampagevote.h, VoteRule): the cutscene
// skip and the rampage vote both take one.
const char *Name(VoteRule rule);                   // "most" / "half" / "all" / "anyone"
const char *Label(VoteRule rule);                  // "75%" / "Half" / "Everyone" / "Anyone"
bool        ParseVoteRule(const std::string &text, VoteRule *out);

// missionEnemies (protocol.h, MissionEnemies), which is a plain number on
// the wire and so has no enum of its own here.
const char *MissionEnemiesName(uint8_t rule);      // "original" / "tougher" / "more"
const char *MissionEnemiesLabel(uint8_t rule);     // "Original" / "Tougher" / "More"

// The ranges the numeric settings are held to. A value outside one in the
// file is ignored, the same as any value that makes no sense.
constexpr uint8_t  CONFIG_MAX_WANTED_MIN      = 1;
constexpr uint16_t CONFIG_INTRO_WAIT_S_MIN    = 10;
constexpr uint16_t CONFIG_INTRO_WAIT_S_MAX    = 600;
constexpr uint16_t CONFIG_BEHIND_S_MIN        = 1;
constexpr uint16_t CONFIG_ABANDONED_S_MIN     = 10;
constexpr uint16_t CONFIG_ABANDONED_S_MAX     = 600;
// What session.h's VEHICLE_RELEASE_MS is, in seconds; server.h holds the two
// to each other.
constexpr uint16_t CONFIG_ABANDONED_S_DEFAULT = 60;

struct ServerConfig {
	uint16_t        port               = 2001;
	bool            friendlyFire       = false;                     // §5.2
	WantedLevelRule wantedLevel        = WantedLevelRule::PerPlayer; // §5.1
	bool            missionFailOnDeath = true;                      // §5.4

	// How far outside one of a mission's checkpoints still counts as there,
	// in centimetres: everybody has to be there before a mission moves on
	// (docs/missions.md 5.6 and 9). 5 m, the number settled on, so a friend
	// parked beside you counts. A start counts 5 m (MISSION_START_RADIUS_M),
	// or this when it is wider. The file says it in metres.
	uint16_t        missionMarginCm    = MISSION_MARGIN_CM_DEFAULT;

	// How a mission's enemies stand up to more than one player, and what each
	// participant after the first adds to them, in percent (docs/missions.md
	// 10.4). Single player's by default.
	uint8_t         missionEnemies     = MISSION_ENEMIES_ORIGINAL;
	uint16_t        missionScale       = MISSION_SCALE_DEFAULT;

	// Whether a player's real ammunition is reported to everyone else
	// (docs/protocol.md 1.9.6). Off by default, which is the behaviour
	// CoopIII has always had: a remote player's gun is handed a fixed
	// thousand rounds and nobody ever sees anyone run dry.
	//
	// This is not a shared inventory. Players carry whatever they like; all
	// this decides is whether their own counts are honest on other screens.
	bool            ammoSync           = false;

	// Whether each player's custom skin from Player Setup is sent to everybody
	// else (docs/protocol.md 1.72). On by default; off, every remote player
	// wears the game's default skin.
	bool            syncCustomSkins    = true;

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

	// CoopIII's own typed cheats (TPTO1..TPTO8, a teleport to a player).
	// Outside missions by default; `always` lets them work during one too.
	CoopCheatMode   coopCheats         = CoopCheatMode::OutsideMissions;

	// What happens to the players' cash. `off`, the default, leaves every
	// machine paying its own player for whatever its engine saw; `own` pays
	// an award to whoever earned it; `shared` is one wallet for everybody.
	MoneyMode       money              = MoneyMode::Off;

	// Who a hidden package counts for (§5.11). `shared` by default.
	PackageMode     hiddenPackages     = PackageMode::Shared;

	// Whether the session's progress - the missions passed, the hidden
	// packages found, the Import/Export and crane lists - is kept in
	// CoopIII-Progress.dat next to server.exe, and picked up again when the
	// server starts (progress.h). On by default: a group that plays over
	// several evenings should not have a server that forgets. Read when the
	// server starts; off, the file is neither read nor written, and is left
	// where it is.
	bool            keepProgress       = true;

	// How many players the session takes, up to MAX_PLAYERS. Lowering it
	// turns nobody out; it only stops the next join.
	uint8_t         maxPlayers         = MAX_PLAYERS;

	// The most stars anybody may have, 1 to 6. Six, the game's own, by
	// default; with the wanted level off it does nothing.
	uint8_t         maxWanted          = WANTED_LEVEL_CEILING;

	// How long a checkpoint waits for the participants who are not in it
	// before the owner's mission goes on without them, in seconds; 0 and
	// checkpoints never wait. And whether one waits in a race, a side job or
	// with a clock on the screen too, which by default it does not
	// (protocol.h, MISSION_FLAG_TIMED_CHECKPOINTS).
	uint16_t        missionCheckpointWaitS  = MISSION_CHECKPOINT_WAIT_MS / 1000;
	bool            missionTimedCheckpoints = false;

	// Bringing people to the mission's owner (protocol.h, S_MissionState): a
	// latecomer further than missionCatchUpM, and, in a mission whose
	// checkpoints do not wait, anybody further than missionBehindM for
	// missionBehindS seconds. 0 m for either is never.
	uint16_t        missionCatchUpM    = MISSION_CATCH_UP_M_DEFAULT;
	uint16_t        missionBehindM     = MISSION_BEHIND_M_DEFAULT;
	uint16_t        missionBehindS     = MISSION_BEHIND_S_DEFAULT;

	// How long a start waits for a player whose game is in a mission of its
	// own, the new game's intro say, before it goes on without them.
	uint16_t        missionIntroWaitS  = MISSION_BUSY_WAIT_MS / 1000;

	// Whether a mission's reward goes to everybody in it (docs/missions.md
	// 12.1, the owner's decision and the default) or to its owner alone.
	bool            missionPayHelpers  = true;

	// How many of the players watching a cutscene have to press skip, and how
	// many have to say yes to a rampage, and for how long it asks.
	VoteRule        cutsceneSkip       = VoteRule::Most;
	VoteRule        rampageVote        = VoteRule::Most;
	uint16_t        rampageVoteS       = RAMPAGE_VOTE_MS / 1000;

	// How long a car the session shares stays after everybody has left it and
	// walked away (session.h, VEHICLE_RELEASE_MS), in seconds.
	uint16_t        abandonedCarS      = CONFIG_ABANDONED_S_DEFAULT;

	// What a player has to give to join, or empty for nothing. protocol.h,
	// C_Password: it keeps strangers out and no more.
	std::string     password;

	// What happens outside the session when the server starts, none of it
	// seen by the players (server/probe.cpp). Asking a what-is-my-IP service
	// for this network's public address, so the window can show what a friend
	// on the internet types; and asking the router over UPnP to forward the
	// port here, closed again when the server stops. Both on by default,
	// because a host who has to find out about port forwarding is a host
	// whose friends never get in.
	bool            lookUpPublicAddress = true;
	bool            openRouterPort      = true;

	// The password as a hello can carry it: control characters out, and no
	// longer than PASSWORD_LEN - 1.
	static std::string CleanPassword(const std::string &text);

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
		       missionMarginCm == other.missionMarginCm &&
		       missionEnemies == other.missionEnemies && missionScale == other.missionScale &&
		       ammoSync == other.ammoSync && syncCustomSkins == other.syncCustomSkins &&
		       rampage == other.rampage &&
		       cheats == other.cheats && coopCheats == other.coopCheats &&
		       money == other.money &&
		       hiddenPackages == other.hiddenPackages && keepProgress == other.keepProgress &&
		       password == other.password &&
		       maxPlayers == other.maxPlayers && maxWanted == other.maxWanted &&
		       missionCheckpointWaitS == other.missionCheckpointWaitS &&
		       missionTimedCheckpoints == other.missionTimedCheckpoints &&
		       missionCatchUpM == other.missionCatchUpM &&
		       missionBehindM == other.missionBehindM &&
		       missionBehindS == other.missionBehindS &&
		       missionIntroWaitS == other.missionIntroWaitS &&
		       missionPayHelpers == other.missionPayHelpers &&
		       cutsceneSkip == other.cutsceneSkip && rampageVote == other.rampageVote &&
		       rampageVoteS == other.rampageVoteS && abandonedCarS == other.abandonedCarS &&
		       lookUpPublicAddress == other.lookUpPublicAddress &&
		       openRouterPort == other.openRouterPort;
	}
	bool operator!=(const ServerConfig &other) const { return !(*this == other); }
};

} // namespace coopiii
