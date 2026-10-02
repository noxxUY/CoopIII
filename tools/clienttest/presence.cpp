// Discord Rich Presence: client/src/presence.h. The frames and the JSON that
// go down Discord's pipe, and when they go, with no pipe and no Discord.

#include "discordapp.h"
#include "presence.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace coopiii;
using namespace coopiii::presence;

namespace {

int g_presenceFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_presenceFailures;
}

bool Has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

void TestTheFrame() {
	std::printf("\nthe frame\n");
	const std::string f = Frame(OP_HANDSHAKE, "{}");
	Check(f.size() == FRAME_HEADER + 2, "eight bytes of header, then the JSON");
	const uint8_t want[] = {0, 0, 0, 0, 2, 0, 0, 0, '{', '}'};
	Check(std::memcmp(f.data(), want, sizeof want) == 0, "opcode and length, little-endian");

	const std::string big = Frame(OP_FRAME, std::string(0x01020304 & 0x3FF, 'x'));
	uint32_t op = 99, length = 0;
	Check(ParseHeader(reinterpret_cast<const uint8_t *>(big.data()), &op, &length) &&
	          op == OP_FRAME && length == (0x01020304 & 0x3FF),
	      "and reads back the same");

	const uint8_t huge[] = {1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
	Check(!ParseHeader(huge, &op, &length), "a length no frame has is refused");
	Check(GetU32(huge + 4) == 0x7FFFFFFFu, "the bytes go lowest first");
}

void TestTheHandshake() {
	std::printf("\nthe handshake\n");
	Check(HandshakeJson(1555279137592311990ull) ==
	          "{\"v\":1,\"client_id\":\"1555279137592311990\"}",
	      "version 1 and the application id as a string");
	Check(DISCORD_APP_ID == 1555279137592311990ull, "the built-in application is CoopIII's");
}

void TestTheEscapes() {
	std::printf("\nwhat goes in a JSON string\n");
	Check(JsonEscape("a\"b\\c") == "a\\\"b\\\\c", "quotes and backslashes are escaped");
	Check(JsonEscape("tab\there") == "tab\\u0009here", "control characters are spelled out");
	Check(JsonEscape("caf\xC3\xA9") == "caf\xC3\xA9", "UTF-8 goes through as it is");
	Check(JsonEscape("bad\xE9!") == "bad?!", "a byte that is not UTF-8 does not");
	Check(JsonEscape("cut\xC3") == "cut?", "nor half of a character at the end");
	Check(JsonEscape("Don't Spank Ma Bitch Up") == "Don't Spank Ma Bitch Up",
	      "a title is left as the game writes it");
}

void TestSinglePlayer() {
	std::printf("\nsingle player\n");
	Activity a;
	a.startUnix = 1700000000;
	const std::string j = ActivityJson(4321, a, 7);
	Check(Has(j, "\"cmd\":\"SET_ACTIVITY\""), "a SET_ACTIVITY");
	Check(Has(j, "\"pid\":4321"), "for this process, which is gta3.exe");
	Check(Has(j, "\"details\":\"Single player\""), "says single player");
	Check(!Has(j, "\"state\""), "and nothing under it");
	Check(Has(j, "\"timestamps\":{\"start\":1700000000}"), "timed from the start");
	Check(Has(j, "\"large_image\":\"logo\"") && Has(j, "\"large_text\":\"CoopIII\""),
	      "with the logo");
	Check(!Has(j, "\"party\""), "and no party");
	Check(Has(j, "\"nonce\":\"7\""), "the nonce goes along");
}

void TestASession() {
	std::printf("\na session\n");
	Activity a;
	a.coop      = true;
	a.players   = 2;
	a.slots     = 8;
	a.startUnix = 1700000100;
	std::string j = ActivityJson(1, a, 1);
	Check(Has(j, "\"details\":\"In a co-op session\""), "says co-op");
	Check(Has(j, "\"state\":\"Free roam (2/8)\""), "free roam, two of eight in the text");
	Check(!Has(j, "\"party\""), "and no party size, which Discord draws \"(2 of 8)\"");

	a.onMission = true;
	a.mission   = "The Exchange";
	j           = ActivityJson(1, a, 2);
	Check(Has(j, "\"state\":\"Mission: The Exchange (2/8)\""), "the mission by its title");

	a.mission.clear();
	Check(State(a) == "On a mission (2/8)", "a mission with no title we know");

	a.slots = 0;
	Check(State(a) == "On a mission", "no count before the welcome says how many fit");
	a.slots   = 4;
	a.players = 9;
	Check(State(a) == "On a mission (4/4)", "never more in it than fit");
	a.players = 0;
	Check(State(a) == "On a mission (1/4)", "and never fewer than us");

	a.mission = std::string(300, 'x');
	a.players = 3;
	const std::string s = State(a);
	Check(s.size() == TEXT_MAX && s.compare(s.size() - 6, 6, " (3/4)") == 0,
	      "a long title is cut, the count kept");

	for (const char *bad : {"\xC2\xB7", "\xE2\x80\x93", "\xE2\x80\x94", " - "})
		Check(!Has(ActivityJson(1, a, 3), bad), "no dot or dash between the parts");

	const std::string addr = ActivityJson(1, a, 4);
	Check(!Has(addr, "host") && !Has(addr, "127.0.0.1"), "nothing about the server");
}

void TestTheClear() {
	std::printf("\nthe clear\n");
	Check(ClearJson(99, 5) == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":99},\"nonce\":\"5\"}",
	      "SET_ACTIVITY with no activity takes ours off");
	Check(IsReady("{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{}}"),
	      "READY is the handshake taken");
	Check(!IsReady("{\"code\":4000,\"message\":\"Invalid Client ID\"}"), "an error is not");
	Check(IsError("{\"cmd\":\"SET_ACTIVITY\",\"evt\":\"ERROR\",\"data\":{}}") &&
	          !IsError("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"data\":{}}"),
	      "and a command it did not take is told from one it did");
}

void TestWhenItSends() {
	std::printf("\nwhen it sends\n");
	Check(ShouldSend(false, false, 1000, 0), "the first time on a pipe, changed or not");
	Check(!ShouldSend(false, true, 100000, 0), "not again when nothing changed");
	Check(!ShouldSend(true, true, 1000 + MIN_GAP_MS - 1, 1000), "a change waits out the gap");
	Check(ShouldSend(true, true, 1000 + MIN_GAP_MS, 1000), "and goes once it is up");
	Check(ShouldSend(true, true, 5000, 0xFFFFFFFFu - 20000), "across the tick count wrapping");
}

void TestTheTimes() {
	std::printf("\nthe timer\n");
	Tracker t;
	Activity a = t.Observe(false, false, nullptr, 1, 0, 1000);
	Check(!a.coop && a.startUnix == 1000 && State(a).empty(), "single player, from the start");
	a = t.Observe(false, true, "Give Me Liberty", 1, 0, 1500);
	Check(!a.onMission && a.mission.empty() && a.startUnix == 1000,
	      "its own missions are not shown");
	a = t.Observe(true, false, nullptr, 2, 8, 2000);
	Check(a.coop && a.startUnix == 2000 && a.players == 2 && a.slots == 8,
	      "a session starts its own timer");
	a = t.Observe(true, true, "The Exchange", 3, 8, 2600);
	Check(a.startUnix == 2000 && a.mission == "The Exchange", "which runs on through a mission");
	a = t.Observe(false, false, nullptr, 1, 8, 3000);
	Check(!a.coop && a.startUnix == 1000 && a.slots == 0, "back to the game's own on leaving");
	a = t.Observe(true, false, nullptr, 2, 8, 4000);
	Check(a.startUnix == 4000, "and a new join starts it again");
}

} // namespace

int RunPresenceTests() {
	TestTheFrame();
	TestTheHandshake();
	TestTheEscapes();
	TestSinglePlayer();
	TestASession();
	TestTheClear();
	TestWhenItSends();
	TestTheTimes();
	return g_presenceFailures;
}
