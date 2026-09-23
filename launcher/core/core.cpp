#include "launcher/core.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace coopiii::launcher {
namespace {

// Absolute file offsets. The exe isn't relocated, image base is 0x400000.
// "ESIOTRUT" is how v1.0 spells the armour cheat; v1.1 and the Steam build
// spell it "ESIOTROT" instead, which is exactly why it works as a version
// anchor.
constexpr long        OFF_VERSION_MAGIC = 0x1F4DF4;
constexpr const char *STR_VERSION_MAGIC = "grandtheftauto3";
constexpr long        OFF_CHEAT_ARMOUR  = 0x1F6618;
constexpr const char *STR_CHEAT_ARMOUR  = "ESIOTRUT";

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

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

} // namespace

// ---- small helpers --------------------------------------------------------

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

// ---- MD5 ------------------------------------------------------------------
//
// RFC 1321. Here rather than pulled in as a dependency because it is forty
// lines and the only thing this project hashes is one 2.3 MB executable.

namespace {

struct Md5 {
	uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
	uint64_t bits     = 0;
	uint8_t  block[64] = {};
	size_t   filled    = 0;

	static uint32_t Rotate(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

	void Transform(const uint8_t *p) {
		static const uint32_t K[64] = {
		    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
		    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
		    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
		    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
		    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
		    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
		    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
		    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
		static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		                          5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
		                          4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		                          6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

		uint32_t m[16];
		for (int i = 0; i < 16; ++i)
			m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
			       (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
			       (static_cast<uint32_t>(p[i * 4 + 3]) << 24);

		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		for (int i = 0; i < 64; ++i) {
			uint32_t f;
			int      g;
			if (i < 16) {
				f = (b & c) | (~b & d);
				g = i;
			} else if (i < 32) {
				f = (d & b) | (~d & c);
				g = (5 * i + 1) % 16;
			} else if (i < 48) {
				f = b ^ c ^ d;
				g = (3 * i + 5) % 16;
			} else {
				f = c ^ (b | ~d);
				g = (7 * i) % 16;
			}
			const uint32_t tmp = d;
			d                  = c;
			c                  = b;
			b                  = b + Rotate(a + f + K[i] + m[g], S[i]);
			a                  = tmp;
		}
		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
	}

	void Update(const uint8_t *data, size_t len) {
		bits += static_cast<uint64_t>(len) * 8;
		while (len > 0) {
			const size_t take = (64 - filled) < len ? (64 - filled) : len;
			std::memcpy(block + filled, data, take);
			filled += take;
			data += take;
			len -= take;
			if (filled == 64) {
				Transform(block);
				filled = 0;
			}
		}
	}

	std::string Finish() {
		const uint64_t total = bits;
		uint8_t        pad   = 0x80;
		Update(&pad, 1);
		pad = 0x00;
		while (filled != 56)
			Update(&pad, 1);
		uint8_t tail[8];
		for (int i = 0; i < 8; ++i)
			tail[i] = static_cast<uint8_t>((total >> (8 * i)) & 0xFF);
		bits = total;   // Update would count the length field itself
		std::memcpy(block + filled, tail, 8);
		Transform(block);

		char out[33];
		for (int i = 0; i < 4; ++i)
			for (int b = 0; b < 4; ++b)
				std::snprintf(out + i * 8 + b * 2, 3, "%02X",
				              static_cast<unsigned>((state[i] >> (8 * b)) & 0xFF));
		return std::string(out, 32);
	}
};

} // namespace

std::string Md5Buffer(const void *data, size_t length) {
	Md5 md5;
	md5.Update(static_cast<const uint8_t *>(data), length);
	return md5.Finish();
}

std::string Md5File(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return std::string();

	Md5     md5;
	uint8_t buf[64 * 1024];
	size_t  n;
	while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
		md5.Update(buf, n);
	std::fclose(fh);
	return md5.Finish();
}

// ---- finding the game -----------------------------------------------------

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

// ---- the checks -----------------------------------------------------------

namespace {

Check CheckGameExe(const std::string &gameDir) {
	Check c;
	const std::string exe = Join(gameDir, "gta3.exe");

	if (!FileExists(exe)) {
		c.state  = Check::State::Fail;
		c.title  = "gta3.exe is not in this folder";
		c.detail = "Point at the folder that holds gta3.exe.";
		return c;
	}

	if (!BytesAt(exe, OFF_VERSION_MAGIC, STR_VERSION_MAGIC)) {
		c.state  = Check::State::Fail;
		c.title  = "This does not look like GTA III";
		c.detail = "The file is named gta3.exe but is not the game.";
		return c;
	}

	const unsigned long long size = FileSize(exe);
	if (BytesAt(exe, OFF_CHEAT_ARMOUR, STR_CHEAT_ARMOUR) && size == GAME_SIZE_BYTES) {
		c.state = Check::State::Ok;
		c.title = "gta3.exe is v1.0 retail";
		return c;
	}

	c.state     = Check::State::Fail;
	c.title     = "gta3.exe is not v1.0 retail";
	c.detail    = "Steam ships a 1.1-lineage exe. CoopIII needs a v1.0 downgrade.";
	c.monoLabel = "Expected MD5";
	c.mono      = GAME_MD5;
	return c;
}

Check CheckAsiLoader(const std::string &gameDir) {
	Check c;
	// If any of these exist, something in the process chain loads .asi files.
	static const char *loaders[] = {"dinput8.dll", "vorbisFile.dll", "dsound.dll",
	                                "winmm.dll",   "ddraw.dll",      "d3d8.dll"};
	for (const char *l : loaders) {
		if (FileExists(Join(gameDir, l))) {
			c.state = Check::State::Ok;
			c.title = "ASI loader found";
			c.meta  = l;
			return c;
		}
	}
	c.state    = Check::State::Fail;
	c.title    = "No ASI loader found";
	c.detail   = "CoopIII.asi would never load. Install Ultimate ASI Loader in the game folder.";
	c.linkText = "Ultimate ASI Loader";
	c.link     = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases";
	return c;
}

Check CheckClient(const std::string &gameDir) {
	Check c;
	// Could be in the game root or under modloader/. See docs/compat.md §3.
	const std::string places[] = {Join(gameDir, "CoopIII.asi"),
	                              Join(gameDir, "scripts\\CoopIII.asi"),
	                              Join(gameDir, "modloader\\CoopIII\\CoopIII.asi")};
	const char       *shown[]  = {"CoopIII.asi", "scripts\\CoopIII.asi",
	                              "modloader\\CoopIII\\CoopIII.asi"};

	for (int i = 0; i < 3; ++i) {
		if (FileExists(places[i])) {
			c.state = Check::State::Ok;
			c.title = "CoopIII.asi installed";
			c.meta  = shown[i];

			// A real trap: a stale .asi next to a newer launcher means the log
			// ends up describing an older build's behaviour.
			const std::string built = Join(ExeDir(), "CoopIII.asi");
			if (FileExists(built) && built != places[i] &&
			    FileSize(built) != FileSize(places[i])) {
				c.detail = "The CoopIII.asi next to this launcher is a different build "
				           "from the installed one.";
			}
			return c;
		}
	}

	c.state  = Check::State::Fail;
	c.title  = "CoopIII.asi is not installed";
	c.detail = "Copy it, and CoopIII.ini, next to gta3.exe.";
	return c;
}

Check CheckIni(const std::string &gameDir) {
	Check c;
	if (FileExists(Join(gameDir, "CoopIII.ini"))) {
		c.state = Check::State::Ok;
		c.title = "CoopIII.ini found";
		return c;
	}
	// Informational only: the launcher writes it on start.
	c.state = Check::State::Info;
	c.title = "CoopIII.ini will be created on start";
	return c;
}

} // namespace

Checks RunChecks(const std::string &gameDir) {
	Checks out;
	out.gameDir = gameDir;

	if (gameDir.empty() || !DirExists(gameDir)) {
		Check c;
		c.state  = Check::State::Fail;
		c.title  = gameDir.empty() ? "No game folder chosen" : "That folder is not there";
		c.detail = "Browse to the folder GTA III is installed in.";
		out.items.push_back(c);
		out.problems = 1;
		return out;
	}

	out.items.push_back(CheckGameExe(gameDir));
	out.items.push_back(CheckAsiLoader(gameDir));
	out.items.push_back(CheckClient(gameDir));
	out.items.push_back(CheckIni(gameDir));

	for (const Check &c : out.items)
		if (c.Failed())
			++out.problems;
	return out;
}

// ---- config ---------------------------------------------------------------

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

	// A file we are creating gets the section header, so the result reads like
	// the CoopIII.ini in the repo rather than three bare keys.
	if (text.empty())
		text = "; CoopIII. Written by the launcher; safe to edit by hand.\n\n[CoopIII]\n";

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

void ReadIni(const std::string &path, std::string *host, uint16_t *port, std::string *nick) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return;

	std::string text;
	char        buf[4096];
	size_t      n;
	while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
		text.append(buf, n);
	std::fclose(fh);

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
		if (host && _stricmp(key.c_str(), "host") == 0)
			*host = value;
		else if (nick && _stricmp(key.c_str(), "nick") == 0)
			*nick = value;
		else if (port && _stricmp(key.c_str(), "port") == 0) {
			uint16_t parsed = 0;
			if (ParsePort(value, &parsed))
				*port = parsed;
		}
	}
}

// ---- validation -----------------------------------------------------------

std::string SanitizeNick(const std::string &raw) {
	std::string out;
	out.reserve(raw.size());
	for (char ch : raw)
		if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7F)
			out += ch;

	out = Trim(out);
	// NICK_LEN is 24 including the terminator, so 23 characters travel.
	if (out.size() > 23)
		out.resize(23);
	return out;
}

bool ParsePort(const std::string &text, uint16_t *out) {
	const std::string t = Trim(text);
	if (t.empty() || t.size() > 5)
		return false;
	unsigned value = 0;
	for (char ch : t) {
		if (ch < '0' || ch > '9')
			return false;
		value = value * 10 + static_cast<unsigned>(ch - '0');
	}
	if (value == 0 || value > 65535)
		return false;
	if (out)
		*out = static_cast<uint16_t>(value);
	return true;
}

bool ValidHost(const std::string &text) {
	const std::string t = Trim(text);
	if (t.empty() || t.size() > 255)
		return false;
	for (char ch : t) {
		const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
		                   (ch >= 'A' && ch <= 'Z');
		if (!alnum && ch != '.' && ch != '-' && ch != '_' && ch != ':')
			return false;
	}
	// A bare scheme or a path is the mistake worth catching, and both are
	// already out by the character rule above; this catches "host:port" typed
	// into the host box, which is a different field here.
	return t.find(':') == std::string::npos;
}

// ---- starting the game ----------------------------------------------------

bool LaunchGame(const std::string &gameDir, std::string *error) {
	const std::string exe = Join(gameDir, "gta3.exe");

	// docs/roadmap.md §5.6: the mod only activates when the game was started
	// from here. The marker rides in the child's environment, which
	// CreateProcess children inherit, so the .asi can tell the difference
	// without any IPC.
	if (!SetEnvironmentVariableA(ENV_LAUNCHED, ENV_LAUNCHED_VALUE)) {
		if (error)
			*error = "could not set the co-op marker in the environment";
		return false;
	}

	STARTUPINFOA        si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	std::string         cmd = "\"" + exe + "\"";

	// Working directory has to be the game folder: GTA III resolves its data
	// paths relative to it.
	const BOOL ok = CreateProcessA(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
	                               gameDir.c_str(), &si, &pi);

	// Out of the parent's own environment again, so anything else this process
	// starts later is not accidentally a co-op session.
	SetEnvironmentVariableA(ENV_LAUNCHED, nullptr);

	if (!ok) {
		if (error) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "could not start gta3.exe (error %lu)",
			              GetLastError());
			*error = buf;
		}
		return false;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

} // namespace coopiii::launcher
