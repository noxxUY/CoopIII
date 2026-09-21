// CoopIII launcher.
//
// Job here is to catch the things that can go wrong before the game even
// starts, and fail here with a plain sentence, rather than twenty seconds
// later inside some hook:
//
//   1. the game is not where we think it is
//   2. the exe is not the build CoopIII targets
//   3. there is no ASI loader, so CoopIII.asi would never be loaded at all
//   4. CoopIII.asi is missing, or is an older build than this launcher
//   5. the player has not said which server to join
//
// Does NOT inject itself. Player already has an ASI loader
// (docs/compat.md §2.1), and letting that do the loading keeps CoopIII a
// well-behaved guest next to their other mods. So this just checks things,
// reports what it found, and starts the game.
//
//   coopiii-launcher [--game <dir>] [--server <host[:port]>] [--nick <name>]
//                    [--check]

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace {

// Kept in step with client/src/game/addresses.h. Duplicated, not shared,
// since the launcher shouldn't pull in the client's engine headers. A drift
// between the two would let a wrong exe slip through though, so both cite
// the same build.
constexpr unsigned long GAME_SIZE_BYTES = 2383872;
constexpr const char   *GAME_MD5        = "85414BF9EB414D00AD81062360F0DB1F";

// Absolute file offsets. The exe isn't relocated, image base is 0x400000.
// "ESIOTRUT" is how v1.0 spells the armour cheat; v1.1 and the Steam build
// spell it "ESIOTROT" instead, which is exactly why it works as a version
// anchor.
constexpr long        OFF_VERSION_MAGIC = 0x1F4DF4;
constexpr const char *STR_VERSION_MAGIC = "grandtheftauto3";
constexpr long        OFF_CHEAT_ARMOUR  = 0x1F6618;
constexpr const char *STR_CHEAT_ARMOUR  = "ESIOTRUT";

int g_problems = 0;

void Say(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::printf("\n");
}

void Ok(const char *what)   { Say("  [ ok ] %s", what); }
void Warn(const char *what) { Say("  [warn] %s", what); }
void Fail(const char *what) { Say("  [FAIL] %s", what); ++g_problems; }

bool FileExists(const std::string &path) {
	const DWORD a = GetFileAttributesA(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::string &path) {
	const DWORD a = GetFileAttributesA(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Join(const std::string &dir, const char *leaf) {
	if (dir.empty())
		return leaf;
	const char last = dir[dir.size() - 1];
	return dir + ((last == '\\' || last == '/') ? "" : "\\") + leaf;
}

std::string ExeDir() {
	char        buf[MAX_PATH] = {0};
	const DWORD n             = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

unsigned long long FileSize(const std::string &path) {
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d))
		return 0;
	return (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool BytesAt(const std::string &path, long offset, const char *expect) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return false;
	const size_t      len = std::strlen(expect);
	std::vector<char> buf(len);
	const bool        ok = std::fseek(fh, offset, SEEK_SET) == 0 &&
	                std::fread(buf.data(), 1, len, fh) == len &&
	                std::memcmp(buf.data(), expect, len) == 0;
	std::fclose(fh);
	return ok;
}

// ---- finding the game -----------------------------------------------------

// Usual install spots, roughly in the order a player is likely to have used.
// Not meant to be exhaustive. --game exists precisely because guessing only
// gets you so far, and a wrong guess that silently "works" is worse than
// just asking.
std::vector<std::string> CandidateGameDirs() {
	std::vector<std::string> dirs;

	// Beside the launcher itself, if the whole CoopIII folder got dropped
	// straight into the game directory.
	dirs.push_back(ExeDir());

	static const char *roots[] = {
	    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Grand Theft Auto 3",
	    "C:\\Program Files\\Steam\\steamapps\\common\\Grand Theft Auto 3",
	    "C:\\Program Files (x86)\\Rockstar Games\\GTAIII",
	    "C:\\Program Files\\Rockstar Games\\GTAIII",
	    "C:\\Program Files (x86)\\Grand Theft Auto 3",
	    "C:\\GTA3",
	};
	for (const char *r : roots)
		dirs.push_back(r);

	// Worth a shot: a Steam library living on another drive isn't rare.
	for (char drive = 'D'; drive <= 'K'; ++drive) {
		std::string s = "X:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto 3";
		s[0]          = drive;
		dirs.push_back(s);
	}
	return dirs;
}

std::string FindGameDir() {
	for (const std::string &d : CandidateGameDirs())
		if (DirExists(d) && FileExists(Join(d, "gta3.exe")))
			return d;
	return std::string();
}

// ---- checks ---------------------------------------------------------------

void CheckGameExe(const std::string &gameDir) {
	const std::string exe = Join(gameDir, "gta3.exe");
	Say("game: %s", exe.c_str());

	if (!FileExists(exe)) {
		Fail("gta3.exe is not here");
		return;
	}

	const unsigned long long size = FileSize(exe);
	if (size != GAME_SIZE_BYTES) {
		Say("  [FAIL] gta3.exe is %llu bytes; CoopIII targets v1.0 retail, "
		    "which is %lu",
		    size, GAME_SIZE_BYTES);
		++g_problems;
	} else {
		Ok("gta3.exe is the right size for v1.0 retail");
	}

	if (!BytesAt(exe, OFF_VERSION_MAGIC, STR_VERSION_MAGIC)) {
		Fail("this does not look like GTA III at all");
		return;
	}

	if (BytesAt(exe, OFF_CHEAT_ARMOUR, STR_CHEAT_ARMOUR)) {
		Ok("version anchor says v1.0 retail");
	} else {
		Say("  [FAIL] this is GTA III, but not v1.0. CoopIII's addresses "
		    "belong to that build only");
		Say("         expected MD5 %s", GAME_MD5);
		Say("         Steam ships a 1.1-lineage exe; CoopIII targets a v1.0 "
		    "downgrade");
		++g_problems;
	}
}

void CheckAsiLoader(const std::string &gameDir) {
	// If any of these exist, something in the process chain loads .asi files.
	static const char *loaders[] = {"dinput8.dll", "vorbisFile.dll", "dsound.dll",
	                                "winmm.dll",   "ddraw.dll",      "d3d8.dll"};
	for (const char *l : loaders) {
		if (FileExists(Join(gameDir, l))) {
			Say("  [ ok ] ASI loader present (%s)", l);
			return;
		}
	}
	Fail("no ASI loader found, so CoopIII.asi would never be loaded");
	Say("         install Ultimate ASI Loader into the game folder");
}

void CheckClient(const std::string &gameDir) {
	// Could be in the game root or under modloader/. See compat.md §3.
	const std::string places[] = {Join(gameDir, "CoopIII.asi"),
	                              Join(gameDir, "scripts\\CoopIII.asi"),
	                              Join(gameDir, "modloader\\CoopIII\\CoopIII.asi")};
	std::string found;
	for (const std::string &p : places)
		if (FileExists(p)) {
			found = p;
			break;
		}

	if (found.empty()) {
		Fail("CoopIII.asi is not installed in the game folder");
		Say("         copy it, and CoopIII.ini, next to gta3.exe");
		return;
	}
	Say("  [ ok ] client: %s", found.c_str());

	// This one's a real trap: a stale .asi next to a newer launcher means the
	// log ends up describing an older build's behaviour, which is confusing
	// to debug.
	const std::string built = Join(ExeDir(), "CoopIII.asi");
	if (FileExists(built) && built != found && FileSize(built) != FileSize(found))
		Warn("the CoopIII.asi next to this launcher differs from the installed "
		     "one, so you may be about to run an older build");

	if (!FileExists(Join(gameDir, "CoopIII.ini")))
		Warn("no CoopIII.ini, so the client will default to 127.0.0.1:2001 as "
		     "\"Player\"");
}

// ---- config ---------------------------------------------------------------

// Only rewrites the keys we were actually given; everything else, comments
// included, is left alone. A launcher that flattens a hand-edited config
// just broke something the player cared about.
bool UpdateIni(const std::string &path, const std::string &host, int port,
               const std::string &nick) {
	std::string text;
	if (FILE *fh = std::fopen(path.c_str(), "rb")) {
		char   buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
			text.append(buf, n);
		std::fclose(fh);
	}

	auto replaceKey = [&text](const char *key, const std::string &value) {
		const size_t keyLen = std::strlen(key);
		size_t       pos    = 0;
		while (pos < text.size()) {
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos)
				eol = text.size();

			size_t b = pos;
			while (b < eol && (text[b] == ' ' || text[b] == '\t'))
				++b;

			if (b + keyLen <= eol && _strnicmp(text.c_str() + b, key, keyLen) == 0) {
				size_t after = b + keyLen;
				while (after < eol && (text[after] == ' ' || text[after] == '\t'))
					++after;
				if (after < eol && text[after] == '=') {
					text.replace(b, eol - b, std::string(key) + " = " + value);
					return;
				}
			}
			pos = eol + 1;
		}
		// Key wasn't there, so append instead of failing. Keeps a trimmed-down
		// ini working.
		if (!text.empty() && text[text.size() - 1] != '\n')
			text += "\n";
		text += std::string(key) + " = " + value + "\n";
	};

	if (!host.empty())
		replaceKey("host", host);
	if (port > 0)
		replaceKey("port", std::to_string(port));
	if (!nick.empty())
		replaceKey("nick", nick);

	FILE *out = std::fopen(path.c_str(), "wb");
	if (!out)
		return false;
	std::fwrite(text.data(), 1, text.size(), out);
	std::fclose(out);
	return true;
}

bool LaunchGame(const std::string &gameDir) {
	const std::string exe = Join(gameDir, "gta3.exe");

	STARTUPINFOA        si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};

	std::string cmd = "\"" + exe + "\"";
	// Working directory has to be the game folder, because GTA III resolves its
	// data paths relative to it.
	if (!CreateProcessA(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
	                    gameDir.c_str(), &si, &pi)) {
		Say("could not start gta3.exe (error %lu)", GetLastError());
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

void Usage() {
	Say("CoopIII launcher");
	Say("");
	Say("  --game <dir>            where GTA III is installed");
	Say("  --server <host[:port]>  server to join");
	Say("  --nick <name>           your name in game");
	Say("  --check                 run the checks and stop");
	Say("");
	Say("With no arguments it finds the game, checks it, and starts it.");
}

} // namespace

int main(int argc, char **argv) {
	std::string gameDir, server, nick;
	bool        launch = true;

	for (int i = 1; i < argc; ++i) {
		const char *a    = argv[i];
		auto        next = [&]() -> const char * {
            return i + 1 < argc ? argv[++i] : nullptr;
		};

		if (!std::strcmp(a, "--game")) {
			if (const char *v = next())
				gameDir = v;
		} else if (!std::strcmp(a, "--server")) {
			if (const char *v = next())
				server = v;
		} else if (!std::strcmp(a, "--nick")) {
			if (const char *v = next())
				nick = v;
		} else if (!std::strcmp(a, "--check") || !std::strcmp(a, "--no-launch")) {
			launch = false;
		} else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
			Usage();
			return 0;
		} else {
			Say("unknown option: %s", a);
			Usage();
			return 2;
		}
	}

	Say("CoopIII launcher");
	Say("");

	if (gameDir.empty()) {
		gameDir = FindGameDir();
		if (gameDir.empty()) {
			Say("Could not find GTA III.");
			Say("Point at it:  coopiii-launcher --game \"C:\\path\\to\\GTA3\"");
			return 1;
		}
	}
	if (!DirExists(gameDir)) {
		Say("No such directory: %s", gameDir.c_str());
		return 1;
	}

	CheckGameExe(gameDir);
	CheckAsiLoader(gameDir);
	CheckClient(gameDir);

	if (!server.empty() || !nick.empty()) {
		std::string  host  = server;
		int          port  = 0;
		const size_t colon = server.find(':');
		if (colon != std::string::npos) {
			host = server.substr(0, colon);
			port = std::atoi(server.c_str() + colon + 1);
		}
		const std::string ini = Join(gameDir, "CoopIII.ini");
		if (UpdateIni(ini, host, port, nick))
			Say("  [ ok ] updated %s", ini.c_str());
		else
			Warn("could not write CoopIII.ini");
	}

	Say("");
	if (g_problems > 0) {
		Say("%d problem%s, not starting the game.", g_problems,
		    g_problems == 1 ? "" : "s");
		Say("Fix the [FAIL] lines above and run this again.");
		return 1;
	}
	Say("Everything checks out.");

	if (!launch)
		return 0;

	Say("Starting GTA III...");
	if (!LaunchGame(gameDir))
		return 1;

	Say("");
	Say("If anything goes wrong, read CoopIII.log in the game folder for what "
	    "the client did and why it stopped.");
	return 0;
}
