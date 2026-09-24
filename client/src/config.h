// CoopIII.ini, read from next to the .asi.
//
// Kept to the few things a player actually needs to set. Anything that's a
// protocol constant (snapshot rate, max players, channel layout) lives in
// protocol.h instead and isn't configurable - that's contract, not preference.
#pragma once

#include <coopiii/protocol.h>

#include <string>

namespace coopiii {

struct Config {
	std::string host = "127.0.0.1";
	uint16_t    port = DEFAULT_PORT;
	std::string nick = "Player";

	// Written to CoopIII.log next to the .asi. On by default - with this many
	// plugins patching the same binary (docs/compat.md), the log is usually
	// the only thing that explains a failed start.
	bool logToFile = true;

	// Off by default. In a session a pause is a desync and basically an
	// exploit: everyone else keeps playing, so the pausing player sees a
	// frozen city and then a jump, and anyone being shot at can stop time
	// just by opening the menu. The menu still looks and sounds paused -
	// see client/src/game/pause.h for what that separation costs and where
	// it happens. Turn this on to get stock single-player pause behaviour.
	bool menuPausesTheGame = false;

	// How big a nametag is drawn, as a multiple of the built-in size. The
	// built-in one is a fixed fraction of screen height and is the same on
	// every resolution, so this is a preference about how prominent other
	// players' names should be, not a correction for a screen. Clamped to
	// something a person could still read at one end and still see past at
	// the other.
	float nametagScale = 1.0f;

	// Which key asks for a passenger seat in somebody else's car. A letter, a
	// digit or F1 to F12 in the ini, stored as its virtual-key code. GTA III has
	// no binding of its own for this, so there is nothing to clash with inside
	// the game - but plenty outside it, which is why it is settable.
	int seatKey = 'G';

	// The chat: one key opens a line to type, Enter sends it and Escape drops
	// it; the other shows or hides the list of who is in the session. Spelled
	// the way seatKey is.
	int chatKey = 'T';
	int listKey = 0x78;   // F9

	// The version in the bottom-left corner of the HUD. On by default, since
	// the first thing anybody reporting a bug gets asked is which build.
	bool showVersion = true;

	// The server's password, when it has one (protocol.h, C_Password). Empty
	// sends nothing, which is what a server without one expects.
	std::string password;

	// A key as the ini spells it, as a virtual-key code: one letter or digit,
	// or F1 to F12. Zero for anything else. Exposed for tests.
	static int ParseKey(const std::string &value);

	// Parses INI text. Unknown keys are ignored, not fatal, so a config from
	// a newer build still loads. Returns false only if `text` is empty.
	bool ParseIni(const std::string &text);

	// Reads `path`. A missing file isn't an error - the defaults above are a
	// working single-machine setup, and demanding config before the mod will
	// even start makes for a bad first run.
	bool LoadFromFile(const std::string &path);

	// Environment overrides, applied after the file so they win.
	//
	// These exist for the multi-instance case, nothing else. CoopIII.ini is
	// only ever read, never written, so two copies of the game sharing one
	// file is harmless - but they'd then share a nick, and two players both
	// called "Player" in one session isn't a working test. The launcher (or
	// a test script) sets COOPIII_NICK per process instead of maintaining
	// two ini files and swapping them in and out of one game folder.
	//
	//   COOPIII_HOST, COOPIII_PORT, COOPIII_NICK
	void ApplyEnvOverrides();

	// Same thing but with values passed in directly, so it's testable
	// without touching the process environment. Null or empty leaves the
	// field alone; an unparseable port is just ignored.
	void ApplyOverrides(const char *host, const char *port, const char *nick);

	// Where CoopIII.ini is read from: COOPIII_INI if set, otherwise beside
	// the .asi. That way two instances out of one game folder can be given
	// different configs without either editing the other's.
	static std::string IniPath();

	// Trims, strips control characters, truncates to NICK_LEN-1 so the nick
	// always fits C_Hello::nick with room for the terminator. Applied by
	// ParseIni; exposed here for tests.
	static std::string SanitizeNick(const std::string &raw);

	// Absolute path to `filename` in the directory holding this module, so
	// config and log sit next to the .asi rather than in the process CWD
	// (which the game changes).
	static std::string PathNextToModule(const char *filename);
};

} // namespace coopiii
