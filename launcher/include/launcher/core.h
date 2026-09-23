// Everything the launcher knows how to do, with no interface attached.
//
// The CLI and the window are two front ends over this, so a check can only
// ever have one answer and the installer can reuse the same ones rather than
// growing a second opinion about what a good GTA III folder looks like.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coopiii::launcher {

// The build CoopIII targets. Kept in step with client/src/game/addresses.h -
// duplicated rather than shared, since the launcher should not pull in the
// client's engine headers. A drift between the two would let a wrong exe slip
// through, so both cite the same build.
constexpr unsigned long long GAME_SIZE_BYTES = 2383872;
constexpr const char        *GAME_MD5        = "85414BF9EB414D00AD81062360F0DB1F";

// The marker the launcher puts in the game's environment, and the client looks
// for before it installs anything - docs/roadmap.md §5.6. Environment blocks
// are inherited by CreateProcess children, so this needs no IPC and cannot be
// set by accident.
constexpr const char *ENV_LAUNCHED = "COOPIII_LAUNCHED";
constexpr const char *ENV_LAUNCHED_VALUE = "1";

// ---- checks ---------------------------------------------------------------

struct Check {
	enum class State { Ok, Fail, Info };

	State       state = State::Ok;
	std::string title;       // the one-line result
	std::string detail;      // what to do about it, when it failed
	std::string meta;        // a short right-hand note, e.g. "dinput8.dll"
	std::string monoLabel;   // e.g. "Expected MD5"
	std::string mono;        // the value under it, in Fragment Mono
	std::string linkText;    // a phrase inside `detail` that is a link
	std::string link;        // where it goes

	bool Failed() const { return state == State::Fail; }
};

struct Checks {
	std::string        gameDir;
	bool               foundAutomatically = false;
	std::vector<Check> items;
	int                problems = 0;

	bool Ready() const { return problems == 0 && !gameDir.empty(); }
};

// Usual install spots, in the order a player is likely to have used. Not
// exhaustive: a wrong guess that silently "works" is worse than asking, which
// is what the Browse button is for.
std::vector<std::string> CandidateGameDirs();

// The first candidate that holds a gta3.exe, or empty.
std::string FindGameDir();

// Runs all four checks against `gameDir`. The order is the design's:
// gta3.exe, ASI loader, CoopIII.asi, CoopIII.ini.
Checks RunChecks(const std::string &gameDir);

// ---- config ---------------------------------------------------------------

// Rewrites only the keys given; everything else, comments included, is left
// alone. A launcher that flattens a hand-edited config just broke something
// the player cared about. Creates the file if it is not there.
bool UpdateIni(const std::string &path, const std::string &host, int port,
               const std::string &nick);

// Reads host, port and nick back out, for filling the form in. Missing keys
// leave their argument alone.
void ReadIni(const std::string &path, std::string *host, uint16_t *port, std::string *nick);

// ---- validation -----------------------------------------------------------

// Trims, drops control characters and truncates to what C_Hello::nick can
// carry (NICK_LEN - 1 = 23). The same rule the client applies, so what the
// launcher shows is what other players will see.
std::string SanitizeNick(const std::string &raw);

// A port is 1 to 65535. Returns false for anything else, including empty.
bool ParsePort(const std::string &text, uint16_t *out);

// A server address has to be something ENet can resolve: a host name or an
// IPv4 literal, no scheme, no path, no spaces.
bool ValidHost(const std::string &text);

// ---- starting the game ----------------------------------------------------

// Starts gta3.exe from `gameDir` with the co-op marker in its environment.
// The working directory is the game folder, because GTA III resolves its data
// paths relative to it.
bool LaunchGame(const std::string &gameDir, std::string *error);

// ---- small helpers the front ends share -----------------------------------

std::string Join(const std::string &dir, const char *leaf);
bool        FileExists(const std::string &path);
bool        DirExists(const std::string &path);
std::string ExeDir();

// MD5 of a file, uppercase hex, or empty if it could not be read. Used to tell
// a player which exe they have, and by the installer to prove a downgrade
// landed on the right bytes.
std::string Md5File(const std::string &path);

// The same over bytes already in memory. The downgrader hashes the exe it is
// about to write before it writes it, which is the whole point of the check.
std::string Md5Buffer(const void *data, size_t length);

} // namespace coopiii::launcher
