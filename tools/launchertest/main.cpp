// launcher-core, with no game and no window.
//
// The two things worth protecting here are the ones a player would notice:
// a name that does not survive the trip to the wire, and a hand-edited
// CoopIII.ini that the launcher flattens on the way past.
#include "launcher/core.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

using namespace coopiii::launcher;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what) {
	std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
	if (!ok)
		++g_failures;
}

void CheckEq(const std::string &got, const std::string &want, const char *what) {
	char line[512];
	std::snprintf(line, sizeof(line), "%s -> \"%s\"", what, got.c_str());
	if (got != want)
		std::snprintf(line, sizeof(line), "%s -> \"%s\", wanted \"%s\"", what, got.c_str(),
		              want.c_str());
	Check(got == want, line);
}

std::string TempPath(const char *leaf) {
	char dir[MAX_PATH] = {0};
	GetTempPathA(MAX_PATH, dir);
	return std::string(dir) + leaf;
}

std::string ReadWhole(const std::string &path) {
	std::string out;
	if (FILE *fh = std::fopen(path.c_str(), "rb")) {
		char   buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0)
			out.append(buf, n);
		std::fclose(fh);
	}
	return out;
}

void Write(const std::string &path, const std::string &text) {
	if (FILE *fh = std::fopen(path.c_str(), "wb")) {
		std::fwrite(text.data(), 1, text.size(), fh);
		std::fclose(fh);
	}
}

// ---- names ----------------------------------------------------------------

void TestNick() {
	std::printf("names\n");

	CheckEq(SanitizeNick("noxx"), "noxx", "a plain name is left alone");
	CheckEq(SanitizeNick("  noxx  "), "noxx", "surrounding space is trimmed");
	CheckEq(SanitizeNick("no\txx"), "noxx", "a tab is dropped");
	CheckEq(SanitizeNick("no\nxx"), "noxx", "a newline is dropped");

	// NICK_LEN is 24 in the protocol, so 23 characters plus a terminator
	// travel. A name that is silently cut on the wire is a name other players
	// see differently from the one its owner typed.
	const std::string longName = "abcdefghijklmnopqrstuvwxyz";
	CheckEq(SanitizeNick(longName), "abcdefghijklmnopqrstuvw",
	        "a 26 character name is cut to 23");
	Check(SanitizeNick(longName).size() == 23, "23 characters is the limit");
	Check(SanitizeNick("     ").empty(), "a name of only spaces comes back empty");
}

// ---- ports and hosts ------------------------------------------------------

void TestPortAndHost() {
	std::printf("ports and hosts\n");

	uint16_t port = 0;
	Check(ParsePort("2001", &port) && port == 2001, "2001 parses");
	Check(ParsePort("1", &port) && port == 1, "1 parses");
	Check(ParsePort("65535", &port) && port == 65535, "65535 parses");
	Check(!ParsePort("0", &port), "0 is not a port");
	Check(!ParsePort("65536", &port), "65536 is out of range");
	Check(!ParsePort("", &port), "an empty port is refused");
	Check(!ParsePort("20o1", &port), "a letter in the port is refused");
	Check(!ParsePort("-1", &port), "a negative port is refused");
	Check(!ParsePort(" 2001", &port) == false, "surrounding space is tolerated");

	Check(ValidHost("192.168.1.40"), "an IPv4 literal is a host");
	Check(ValidHost("coop.example.com"), "a host name is a host");
	Check(ValidHost("localhost"), "localhost is a host");
	Check(!ValidHost(""), "an empty host is refused");
	Check(!ValidHost("192.168.1.40:2001"), "host:port belongs in two fields, not one");
	Check(!ValidHost("http://example.com"), "a URL is not a host");
	Check(!ValidHost("has a space"), "a space is refused");
}

// ---- CoopIII.ini ----------------------------------------------------------

void TestIni() {
	std::printf("CoopIII.ini\n");

	const std::string path = TempPath("coopiii-launchertest.ini");
	DeleteFileA(path.c_str());

	// A file the player has edited: comments, their own spacing, and a key the
	// launcher does not know about.
	const std::string original =
	    "; my config, do not eat\n"
	    "[CoopIII]\n"
	    "host = 10.0.0.9\n"
	    "port = 2001\n"
	    "nick = marta\n"
	    "\n"
	    "; I like my nametags big\n"
	    "nametagScale = 2.0\n"
	    "seatKey = H\n";
	Write(path, original);

	Check(UpdateIni(path, "192.168.1.40", 2010, "noxx"), "the ini is written");
	const std::string after = ReadWhole(path);

	Check(after.find("; my config, do not eat") != std::string::npos,
	      "the leading comment survives");
	Check(after.find("; I like my nametags big") != std::string::npos,
	      "a comment in the middle survives");
	Check(after.find("nametagScale = 2.0") != std::string::npos,
	      "a key the launcher does not know is left alone");
	Check(after.find("seatKey = H") != std::string::npos, "and so is seatKey");
	Check(after.find("host = 192.168.1.40") != std::string::npos, "host is updated");
	Check(after.find("port = 2010") != std::string::npos, "port is updated");
	Check(after.find("nick = noxx") != std::string::npos, "nick is updated");
	Check(after.find("10.0.0.9") == std::string::npos, "the old host is gone");
	Check(after.find("marta") == std::string::npos, "the old nick is gone");

	// Read it back.
	std::string host = "x", nick = "x";
	uint16_t    port = 1;
	ReadIni(path, &host, &port, &nick);
	CheckEq(host, "192.168.1.40", "host reads back");
	CheckEq(nick, "noxx", "nick reads back");
	Check(port == 2010, "port reads back");

	// A missing file is created, and reads like a config rather than three
	// bare lines.
	DeleteFileA(path.c_str());
	Check(UpdateIni(path, "127.0.0.1", 2001, "Player"), "a missing ini is created");
	const std::string fresh = ReadWhole(path);
	Check(fresh.find("[CoopIII]") != std::string::npos, "the new file has its section header");
	Check(fresh.find("host = 127.0.0.1") != std::string::npos, "and the keys");

	// A file with no keys at all still ends up with them.
	Write(path, "; nothing but a comment\n");
	Check(UpdateIni(path, "1.2.3.4", 1234, "kiko"), "a comment-only ini is updated");
	const std::string appended = ReadWhole(path);
	Check(appended.find("; nothing but a comment") != std::string::npos,
	      "the comment is still there");
	Check(appended.find("host = 1.2.3.4") != std::string::npos, "and the key was appended");

	DeleteFileA(path.c_str());
}

// ---- MD5 ------------------------------------------------------------------

void TestMd5() {
	std::printf("MD5\n");

	const std::string path = TempPath("coopiii-md5test.bin");

	Write(path, "");
	CheckEq(Md5File(path), "D41D8CD98F00B204E9800998ECF8427E", "MD5 of an empty file");

	Write(path, "abc");
	CheckEq(Md5File(path), "900150983CD24FB0D6963F7D28E17F72", "MD5 of \"abc\"");

	Write(path, "The quick brown fox jumps over the lazy dog");
	CheckEq(Md5File(path), "9E107D9D372BB6826BD81D3542A419D6", "MD5 of the fox");

	// Longer than one 64-byte block, and not a multiple of it.
	std::string long_(1000, 'a');
	Write(path, long_);
	CheckEq(Md5File(path), "CABE45DCC9AE5B66BA86600CCA6B8BA8", "MD5 of 1000 a's");

	Check(Md5File(TempPath("coopiii-does-not-exist.bin")).empty(),
	      "a file that is not there hashes to nothing");

	DeleteFileA(path.c_str());
}

// ---- the checks -----------------------------------------------------------

void TestChecks() {
	std::printf("checks\n");

	const Checks none = RunChecks("");
	Check(none.problems == 1 && !none.Ready(), "no folder is one problem, and not ready");

	const Checks missing = RunChecks("Z:\\definitely\\not\\here");
	Check(missing.problems == 1 && !missing.Ready(), "a folder that is not there is a problem");

	// An empty directory: gta3.exe missing, no loader, no .asi, no ini. Three
	// failures, and the ini is informational because the launcher writes it.
	char dir[MAX_PATH] = {0};
	GetTempPathA(MAX_PATH, dir);
	const std::string empty = std::string(dir) + "coopiii-checktest";
	CreateDirectoryA(empty.c_str(), nullptr);

	const Checks blank = RunChecks(empty);
	Check(blank.items.size() == 4, "an empty folder still runs all four checks");
	Check(blank.problems == 3, "three of them fail");
	Check(!blank.Ready(), "and it is not ready to start");
	if (blank.items.size() == 4) {
		Check(blank.items[3].state == Check::State::Info,
		      "a missing CoopIII.ini is informational, not a failure");
		Check(!blank.items[0].detail.empty(), "a failing check says what to do about it");
	}

	RemoveDirectoryA(empty.c_str());
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	TestNick();
	TestPortAndHost();
	TestIni();
	TestMd5();
	TestChecks();

	std::printf("\n%s\n",
	            g_failures == 0 ? "all launcher checks passed" : "launcher checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
