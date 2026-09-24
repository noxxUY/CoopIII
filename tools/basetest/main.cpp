// Unit tests for the client-side pieces that have no engine dependency:
// config parsing and its environment overrides, the log's per-process file
// naming, the network clock, the send-rate limiter and the thread hand-off
// queue.
//
//   xmake build basetest && xmake run basetest

#include "clock.h"
#include "config.h"
#include "log.h"
#include "queue.h"
#include "quat.h"

#include <coopiii/version.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <share.h>
#include <windows.h>
#endif

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// ---- config ---------------------------------------------------------------

void TestConfigDefaults() {
	std::printf("\nconfig defaults\n");
	Config c;
	Check(c.host == "127.0.0.1", "defaults to loopback");
	Check(c.port == DEFAULT_PORT, "defaults to the protocol port");
	Check(c.nick == "Player", "has a usable default nick");
	Check(c.logToFile, "logs by default");

	Check(!c.LoadFromFile("this\\file\\does\\not\\exist.ini"),
	      "a missing file reports false");
	Check(c.host == "127.0.0.1" && c.port == DEFAULT_PORT,
	      "a missing file leaves the defaults intact");
}

void TestConfigParsing() {
	std::printf("\nconfig parsing\n");
	Config c;
	Check(c.ParseIni("[CoopIII]\n"
	                 "; a comment\n"
	                 "# another\n"
	                 "host = 192.168.1.50\n"
	                 "port=2020\n"
	                 "nick =  noxx  \n"
	                 "logToFile = off\n"),
	      "parses a typical file");
	Check(c.host == "192.168.1.50", "reads host");
	Check(c.port == 2020, "reads port");
	Check(c.nick == "noxx", "reads and trims nick");
	Check(!c.logToFile, "reads a boolean");

	Config aliases;
	aliases.ParseIni("server=example.com\nname=bob\n");
	Check(aliases.host == "example.com", "accepts 'server' as an alias for host");
	Check(aliases.nick == "bob", "accepts 'name' as an alias for nick");

	Config caseInsensitive;
	caseInsensitive.ParseIni("HOST=1.2.3.4\nPoRt=9\n");
	Check(caseInsensitive.host == "1.2.3.4" && caseInsensitive.port == 9,
	      "keys are case-insensitive");

	Config unknown;
	Check(unknown.ParseIni("wat = 1\nhost = 10.0.0.1\n"),
	      "an unknown key does not abort the parse");
	Check(unknown.host == "10.0.0.1", "keys after an unknown one still apply");

	Config garbage;
	garbage.ParseIni("port = notanumber\nport = 99999999\n");
	Check(garbage.port == DEFAULT_PORT,
	      "a non-numeric or out-of-range port falls back to the default");

	Config noNewline;
	noNewline.ParseIni("host=5.5.5.5");
	Check(noNewline.host == "5.5.5.5", "last line without a trailing newline");

	Config crlf;
	crlf.ParseIni("host=6.6.6.6\r\nport=7\r\n");
	Check(crlf.host == "6.6.6.6" && crlf.port == 7, "CRLF line endings");

	Check(!Config().ParseIni(""), "empty text reports false");
}

void TestConfigKeys() {
	std::printf("\nconfig keys\n");
	Config d;
	Check(d.seatKey == 'G' && d.chatKey == 'T' && d.listKey == 0x78,
	      "defaults: G for a seat, T to chat, F9 for the list");

	Check(Config::ParseKey("t") == 'T' && Config::ParseKey("7") == '7',
	      "a letter or digit is its own code, uppercased");
	Check(Config::ParseKey("F1") == 0x70 && Config::ParseKey("f12") == 0x7B,
	      "F1 to F12 run from 0x70");
	Check(Config::ParseKey("F0") == 0 && Config::ParseKey("F13") == 0 &&
	          Config::ParseKey("F01") == 0 && Config::ParseKey("Fx") == 0,
	      "an F-number outside 1-12, or not a number, is refused");
	Check(Config::ParseKey("") == 0 && Config::ParseKey("?") == 0 &&
	          Config::ParseKey("Enter") == 0,
	      "punctuation and names without a table are refused");

	Config c;
	c.ParseIni("chatKey = y\nlistKey = F5\nseatKey = F2\n");
	Check(c.chatKey == 'Y' && c.listKey == 0x74 && c.seatKey == 0x71, "reads all three keys");

	Config bad;
	bad.ParseIni("chatKey = Enter\nlistKey = F99\n");
	Check(bad.chatKey == 'T' && bad.listKey == 0x78, "a key it cannot read keeps the default");

	Check(d.scoreboardKey == 0x09, "the scoreboard is on Tab unless the file says otherwise");
	Check(Config::ParseKey("TAB") == 0x09 && Config::ParseKey("tab") == 0x09,
	      "Tab is spelled Tab, any case");
	Config board;
	board.ParseIni("scoreboardKey = F4\n");
	Check(board.scoreboardKey == 0x73 && board.listKey == 0x78,
	      "the scoreboard key is read on its own, and leaves the list key alone");
	Config boardBad;
	boardBad.ParseIni("scoreboardKey = Space\n");
	Check(boardBad.scoreboardKey == 0x09, "and one it cannot read stays on Tab");

	Check(d.showVersion, "the version mark is on by default");
	Config quiet;
	quiet.ParseIni("showVersion = off\n");
	Check(!quiet.showVersion, "and off when the file says so");

	Check(d.password.empty(), "no password unless the file has one");
	Config locked;
	locked.ParseIni("password = let me\x01 in\n");
	Check(locked.password == "let me in", "a password parses, control characters left out");
	locked.ParseIni(std::string("password = ") + std::string(50, 'p') + "\n");
	Check(locked.password.size() == PASSWORD_LEN - 1, "and cut to what the packet carries");
}

// The mark in the corner says the version the build says, read straight out
// of xmake.lua next to this source. Skipped, not failed, where the tree is not
// beside the binary.
std::string ReadFileNear(const char *thisFile, const char *relative) {
	std::string dir = thisFile;
	for (int up = 0; up < 3; ++up) {
		const size_t slash = dir.find_last_of("\\/");
		if (slash == std::string::npos)
			return {};
		dir.resize(slash);
	}
	FILE *fh = std::fopen((dir + "/" + relative).c_str(), "rb");
	if (!fh)
		return {};
	std::string text;
	char        buf[4096];
	size_t      n;
	while ((n = std::fread(buf, 1, sizeof buf, fh)) > 0)
		text.append(buf, n);
	std::fclose(fh);
	return text;
}

void TestTheVersionIsTheBuilds() {
	std::printf("\nthe version in the corner\n");
	Check(std::string(COOPIII_VERSION).find('.') != std::string::npos, "it looks like a version");

	const std::string lua = ReadFileNear(__FILE__, "xmake.lua");
	if (lua.empty()) {
		std::printf("  (xmake.lua is not beside this build; not compared)\n");
		return;
	}
	const std::string want = std::string("set_version(\"") + COOPIII_VERSION + "\")";
	Check(lua.find(want) != std::string::npos, "it is the one xmake.lua builds");

	const std::string json = ReadFileNear(__FILE__, "installer/assets/components.json");
	if (!json.empty()) {
		const std::string key = "\"version\": \"";
		const size_t at  = json.find("\"id\": \"coopiii\"");
		const size_t val = at == std::string::npos ? at : json.find(key, at);
		std::string shipped;
		if (val != std::string::npos) {
			const size_t from = val + key.size();
			shipped = json.substr(from, json.find('"', from) - from);
		}
		Check(shipped == COOPIII_VERSION, "and the one the installer ships");
	}
}

void TestNickSanitising() {
	std::printf("\nnick sanitising\n");
	Check(Config::SanitizeNick("  spaced  ") == "spaced", "trims");
	Check(Config::SanitizeNick("") == "Player", "empty falls back");
	Check(Config::SanitizeNick("   ") == "Player", "whitespace-only falls back");

	// Split literal: "\x02cd" would otherwise be read as one over-long escape.
	const std::string ctrl = Config::SanitizeNick("ab\x01\x02" "cd");
	Check(ctrl == "abcd", "strips control characters");

	const std::string longNick = Config::SanitizeNick(std::string(200, 'x'));
	Check(longNick.size() == NICK_LEN - 1, "truncates to fit C_Hello::nick");
	Check(longNick.size() < NICK_LEN, "leaves room for the terminator");
}

// ---- clock ----------------------------------------------------------------

void TestEnvOverrides() {
	std::printf("\nenvironment overrides - two game instances need two nicks\n");

	// CoopIII.ini is read-only, so two copies of the game can safely share the
	// same one. What they can't share is a nick - two players both called
	// "Player" isn't a working test. So the launcher sets these per process
	// instead of writing two ini files into one game folder.
	Config c;
	c.ParseIni("host=10.0.0.1\nport=2001\nnick=FromFile\n");
	Check(c.host == "10.0.0.1" && c.port == 2001 && c.nick == "FromFile",
	      "the file is read first");

	c.ApplyOverrides("192.168.1.5", "2002", "Second");
	Check(c.host == "192.168.1.5", "COOPIII_HOST wins over the file");
	Check(c.port == 2002, "COOPIII_PORT wins over the file");
	Check(c.nick == "Second", "COOPIII_NICK wins over the file");

	c.ApplyOverrides(nullptr, nullptr, nullptr);
	Check(c.host == "192.168.1.5" && c.port == 2002 && c.nick == "Second",
	      "an unset variable changes nothing");

	c.ApplyOverrides("", "", "");
	Check(c.host == "192.168.1.5" && c.port == 2002 && c.nick == "Second",
	      "and neither does an empty one");

	c.ApplyOverrides(nullptr, "70000", nullptr);
	Check(c.port == 2002, "an out-of-range port is ignored rather than fatal");
	c.ApplyOverrides(nullptr, "banana", nullptr);
	Check(c.port == 2002, "so is an unparseable one");

	c.ApplyOverrides(nullptr, nullptr, "  Trailing spaces and a very long name indeed  ");
	Check(c.nick.size() < NICK_LEN, "an overridden nick is sanitised like a parsed one");
	Check(c.nick[0] != ' ', "and trimmed");
}

void TestLogPathPerProcess() {
	std::printf("\nlog path - two instances must not share one CoopIII.log\n");

	Check(LogPathForProcess("C:\\game\\CoopIII.log", 4312) == "C:\\game\\CoopIII.4312.log",
	      "the pid goes before the extension");
	Check(LogPathForProcess("CoopIII.log", 7) == "CoopIII.7.log",
	      "a bare file name works too");
	Check(LogPathForProcess("C:\\game\\CoopIII", 7) == "C:\\game\\CoopIII.7",
	      "no extension means the pid is just appended");

	// A dot in a directory name is not an extension. Splitting on it blindly
	// would put the log in a directory that doesn't exist.
	Check(LogPathForProcess("C:\\my.mods\\CoopIII", 7) == "C:\\my.mods\\CoopIII.7",
	      "a dotted directory is not mistaken for an extension");
	Check(LogPathForProcess("C:/my.mods/CoopIII", 7) == "C:/my.mods/CoopIII.7",
	      "forward slashes count as separators too");
	Check(LogPathForProcess("C:\\my.mods\\CoopIII.log", 7) == "C:\\my.mods\\CoopIII.7.log",
	      "and a real extension still wins when there is one");

	Check(LogPathForProcess("a.b.c.log", 1) == "a.b.c.1.log", "only the last dot is the extension");

#ifdef _WIN32
	// Now the actual behaviour, not just the naming. Hold the log the way a
	// first game instance would hold it, then ask LogOpen for that same path
	// and check it falls back to the per-pid one. A plain fopen(path, "w")
	// would have succeeded here too, and truncated the other instance's log.
	const std::string wanted = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
	                           "\\coopiii-basetest.log";
	FILE *held = _fsopen(wanted.c_str(), "w", _SH_DENYWR);
	Check(held != nullptr, "the test could take the log first");
	if (held) {
		LogOpen(wanted);
		const std::string got = LogPath();
		Check(!got.empty(), "LogOpen still found somewhere to write");
		Check(got != wanted, "and it was not the file the other instance holds");
		Check(got == LogPathForProcess(wanted, GetCurrentProcessId()),
		      "it was this process's own log");
		LogClose();
		std::fclose(held);
		std::remove(got.c_str());
		std::remove(wanted.c_str());
	}
#endif
}

void TestClock() {
	std::printf("\nwall clock\n");
	WallClock::Start();
	const uint32_t a = WallClock::NowMs();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	const uint32_t b = WallClock::NowMs();
	Check(b >= a, "monotonic");
	Check(b - a >= 20, "advances in real time");
	Check(b - a < 2000, "does not jump wildly");

	const uint32_t epochBefore = WallClock::NowMs();
	WallClock::Start();
	Check(WallClock::NowMs() >= epochBefore,
	      "a second Start() does not reset the epoch");
}

void TestRateLimiter() {
	std::printf("\nrate limiter\n");
	RateLimiter r(SNAPSHOT_HZ);
	Check(r.IntervalMs() == 40, "25 Hz is a 40 ms interval");

	uint32_t now = 1000;
	Check(r.Ready(now), "fires immediately on the first call");
	Check(!r.Ready(now + 10), "does not fire again inside the interval");
	Check(!r.Ready(now + 39), "still not at 39 ms");
	Check(r.Ready(now + 40), "fires at the interval boundary");

	// A frame at 60 FPS is ~16.67 ms, so the cadence has to survive being
	// sampled at times that aren't multiples of 40 (docs/compat.md §2.4).
	RateLimiter frames(SNAPSHOT_HZ);
	int      fired = 0;
	uint32_t t     = 0;
	for (int frame = 0; frame < 600; ++frame) {   // 600 frames at 60 FPS = 10 s
		t = static_cast<uint32_t>(frame * 1000.0 / 60.0);
		if (frames.Ready(t))
			++fired;
	}
	// 10 seconds at 25 Hz is 250 sends; allow a little slack for the frame
	// times never landing exactly on an interval.
	Check(fired >= 245 && fired <= 255, "holds ~25 Hz when sampled at 60 FPS");

	RateLimiter stall(SNAPSHOT_HZ);
	stall.Ready(0);
	Check(stall.Ready(5000), "fires after a long stall");
	Check(!stall.Ready(5001),
	      "a stall does not queue up a burst of catch-up sends");
}

// ---- queue ----------------------------------------------------------------

void TestQueueBasics() {
	std::printf("\nqueue\n");
	SwapQueue<int> q;
	Check(q.Empty(), "starts empty");

	q.Push(1);
	q.Push(2);
	q.Push(3);
	Check(q.Size() == 3, "tracks size");

	std::vector<int> out;
	q.DrainInto(out);
	Check(out.size() == 3, "drains everything");
	Check(out[0] == 1 && out[1] == 2 && out[2] == 3, "preserves order");
	Check(q.Empty(), "is empty after a drain");

	q.DrainInto(out);
	Check(out.size() == 3, "draining an empty queue appends nothing");

	q.Push(4);
	q.DrainInto(out);
	Check(out.size() == 4 && out[3] == 4, "appends to a non-empty destination");

	q.Push(5);
	q.Clear();
	Check(q.Empty(), "clears");
}

void TestQueueThreading() {
	std::printf("\nqueue under two threads\n");
	// Mirrors the real usage: one producer (socket thread), one consumer
	// (game thread, draining once per frame).
	SwapQueue<int>   q;
	constexpr int    kCount = 20000;
	std::vector<int> drained;

	std::thread producer([&] {
		for (int i = 0; i < kCount; ++i)
			q.Push(int(i));
	});

	while (static_cast<int>(drained.size()) < kCount)
		q.DrainInto(drained);

	producer.join();
	q.DrainInto(drained);

	Check(static_cast<int>(drained.size()) == kCount, "nothing is lost");

	bool ordered = true;
	for (int i = 0; i < kCount; ++i)
		if (drained[static_cast<size_t>(i)] != i) {
			ordered = false;
			break;
		}
	Check(ordered, "a single producer's order is preserved");
}

void TestQueueMoveOnly() {
	std::printf("\nqueue with move-only payloads\n");
	// Messages own a vector; they must move through the queue, not copy.
	SwapQueue<std::vector<uint8_t>> q;
	std::vector<uint8_t>            payload{1, 2, 3, 4};
	q.Push(std::move(payload));
	Check(payload.empty(), "Push took ownership rather than copying");

	std::vector<std::vector<uint8_t>> out;
	q.DrainInto(out);
	Check(out.size() == 1 && out[0].size() == 4, "payload survives the hand-off");
}

// ---- rotation on the wire -------------------------------------------------

// Builds a rotation matrix from yaw/pitch/roll the way a basis actually
// composes. Keeps the test data real rotations instead of three vectors that
// just happen to look orthogonal.
void AxesFromEuler(float yaw, float pitch, float roll, Vec3 &r, Vec3 &f, Vec3 &u) {
	const float cy = std::cos(yaw),   sy = std::sin(yaw);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const float cr = std::cos(roll),  sr = std::sin(roll);

	// Z (yaw) * Y (pitch) * X (roll), rows = right/forward/up.
	r = Vec3{cy * cp, sy * cp, -sp};
	f = Vec3{cy * sp * sr - sy * cr, sy * sp * sr + cy * cr, cp * sr};
	u = Vec3{cy * sp * cr + sy * sr, sy * sp * cr - cy * sr, cp * cr};
}

bool VecNear(const Vec3 &a, const Vec3 &b, float eps = 1e-4f) {
	return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps &&
	       std::fabs(a.z - b.z) < eps;
}

void TestQuatRoundTrip() {
	std::printf("\nrotation round trip - a car is not a heading\n");

	// QuatFromAxes picks one of four branches by which diagonal element is
	// largest. The non-trace branches only get hit when a car is on its side
	// or roof - exactly the cases a yaw-only sync would lose. Cover those.
	struct Case {
		const char *what;
		float       yaw, pitch, roll;
	};
	const Case cases[] = {
	    {"identity", 0.0f, 0.0f, 0.0f},
	    {"driving north-east", 0.7854f, 0.0f, 0.0f},
	    {"turned right round", 3.1415f, 0.0f, 0.0f},
	    {"nose up a ramp", 0.0f, 0.5f, 0.0f},
	    {"nose down", 0.0f, -0.5f, 0.0f},
	    {"two wheels on the kerb", 0.0f, 0.0f, 0.6f},
	    {"on its side", 0.0f, 0.0f, 1.5707f},
	    {"on its roof", 0.0f, 0.0f, 3.1415f},
	    {"on its roof, facing the other way", 3.1415f, 0.0f, 3.1415f},
	    {"mid-barrel-roll off a ramp", 2.0f, 0.9f, 2.5f},
	};

	int worst = 0;
	for (const Case &c : cases) {
		Vec3 r, f, u;
		AxesFromEuler(c.yaw, c.pitch, c.roll, r, f, u);

		const Quat q = QuatFromAxes(r, f, u);

		Vec3 r2, f2, u2;
		AxesFromQuat(q, r2, f2, u2);

		const bool ok = VecNear(r, r2) && VecNear(f, f2) && VecNear(u, u2);
		Check(ok, c.what);
		if (!ok)
			++worst;
	}
	Check(worst == 0, "every orientation survives the wire");
}

void TestQuatRobustness() {
	std::printf("\nrotation, hostile input\n");

	// Four floats off a socket are not a unit quaternion any more.
	Quat scaled{0.0f, 0.0f, 0.3826834f * 4.0f, 0.9238795f * 4.0f};
	Vec3 r, f, u;
	AxesFromQuat(scaled, r, f, u);
	const float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
	Check(std::fabs(len - 1.0f) < 1e-4f,
	      "a non-unit quaternion still produces a unit basis");

	// A zero quaternion is what a dropped or zero-filled packet looks like.
	// Falling back to identity is a visible bug; a NaN matrix takes the
	// renderer down with it.
	AxesFromQuat(Quat{0.0f, 0.0f, 0.0f, 0.0f}, r, f, u);
	Check(VecNear(r, Vec3{1.0f, 0.0f, 0.0f}) && VecNear(f, Vec3{0.0f, 1.0f, 0.0f}) &&
	          VecNear(u, Vec3{0.0f, 0.0f, 1.0f}),
	      "a zero quaternion falls back to identity rather than NaN");

	AxesFromQuat(Quat{0.0f, 0.0f, 0.0f, 0.0f}, r, f, u);
	Check(r.x == r.x && f.y == f.y && u.z == u.z, "and nothing is NaN");
}

} // namespace

// Yes and no for the vote before a rampage (game/rampagevote.h).
void TestVoteKeys() {
	std::printf("\nthe rampage vote keys\n");
	const Config d;
	Check(d.voteYesKey == 'Y' && d.voteNoKey == 'N', "Y and N unless the file says otherwise");
	Config c;
	c.ParseIni("voteYesKey = j\nvoteNoKey = F6\n");
	Check(c.voteYesKey == 'J' && c.voteNoKey == 0x75, "a letter and an F-key");
	Config bad;
	bad.ParseIni("voteYesKey = Space\nvoteNoKey = \n");
	Check(bad.voteYesKey == 'Y' && bad.voteNoKey == 'N', "one it can't read keeps the default");

	const std::string ini = ReadFileNear(__FILE__, "CoopIII.ini");
	if (ini.empty()) {
		std::printf("  [skip] CoopIII.ini isn't beside the source\n");
		return;
	}
	Config shipped;
	shipped.ParseIni(ini);
	Check(ini.find("voteYesKey") != std::string::npos && ini.find("voteNoKey") != std::string::npos &&
	          shipped.voteYesKey == 'Y' && shipped.voteNoKey == 'N',
	      "the ini that ships names both and says Y and N");
}

int main() {
	TestVoteKeys();
	TestConfigDefaults();
	TestConfigParsing();
	TestConfigKeys();
	TestTheVersionIsTheBuilds();
	TestNickSanitising();
	TestEnvOverrides();
	TestLogPathPerProcess();
	TestClock();
	TestRateLimiter();
	TestQueueBasics();
	TestQueueThreading();
	TestQueueMoveOnly();
	TestQuatRoundTrip();
	TestQuatRobustness();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
