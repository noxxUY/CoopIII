// A game that loses its display, a player on another island, the safehouse
// door, the wanted level on a nametag and the scoreboard's money and kills:
// the arithmetic of each (game/pause.h, game/nametag.h, game/gates.h,
// boardlayout.h), and the retail bytes the engine half stands on.

#include "boardlayout.h"
#include "game/addresses.h"
#include "game/gates.h"
#include "game/nametag.h"
#include "game/pause.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_gapFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_gapFailures;
}

void TestAwayReason() {
	std::printf("\naway: the window, then the menu\n");
	Check(LocalAwayReason(false, true, PLAYER_AWAY_MENU, PLAYER_AWAY_WINDOW) == PLAYER_AWAY_NONE,
	      "at the game with no menu is not away");
	Check(LocalAwayReason(true, true, PLAYER_AWAY_MENU, PLAYER_AWAY_WINDOW) == PLAYER_AWAY_MENU,
	      "the menu up is the menu");
	Check(LocalAwayReason(true, false, PLAYER_AWAY_MENU, PLAYER_AWAY_WINDOW) == PLAYER_AWAY_WINDOW,
	      "the window gone wins over the menu the windowed-mode plugin opens for it");
	Check(LocalAwayReason(false, false, PLAYER_AWAY_MENU, PLAYER_AWAY_WINDOW) == PLAYER_AWAY_WINDOW,
	      "and without the plugin");
	Check(SanePlayerAway(0) == PLAYER_AWAY_NONE && SanePlayerAway(2) == PLAYER_AWAY_WINDOW &&
	          SanePlayerAway(0xFF) == PLAYER_AWAY_MENU,
	      "a byte off the wire is one of the three");

	Check(RunUnfocusedFrame(true, true, true), "a session game keeps running without a display");
	Check(!RunUnfocusedFrame(true, true, false), "not outside a session: retail stops");
	Check(!RunUnfocusedFrame(true, false, true), "not on the title screen or a load");
	Check(!RunUnfocusedFrame(false, true, true), "not with the pause policy off (menuPausesTheGame)");

	Check(CloseMenuForMission(true, true, false), "the pause menu is shut for the session's mission");
	Check(!CloseMenuForMission(true, true, true), "the safehouse's save menu is not");
	Check(!CloseMenuForMission(true, false, false) && !CloseMenuForMission(false, true, false),
	      "nor is anything with no menu up, or on the title screen");

	Check(StateOf(false, 80.0f, false, 0, 0, PLAYER_AWAY_WINDOW) == BoardState::Tabbed &&
	          StateOf(false, 80.0f, false, 0, 0, PLAYER_AWAY_MENU) == BoardState::Paused,
	      "the board tells the window from the menu");
	Check(StateOf(false, 80.0f, false, 0, QUIET_AFTER_MS, PLAYER_AWAY_WINDOW) == BoardState::Away,
	      "a player gone quiet is still quiet first");
	char s[32];
	StateLabel(s, sizeof s, BoardState::Tabbed);
	Check(std::strcmp(s, "AWAY") == 0, "the window's is AWAY");

	std::snprintf(s, sizeof s, "{ 64");
	TagPaused(s, sizeof s, true);
	Check(std::strcmp(s, "{ 64 AWAY") == 0, "the tag says AWAY after the health for the window");
	std::snprintf(s, sizeof s, "{ 64");
	TagPaused(s, sizeof s);
	Check(std::strcmp(s, "{ 64 PAUSED") == 0, "and PAUSED for the menu, as before");
}

void TestWantedTag() {
	std::printf("\nthe wanted level on a nametag: a number beside the star\n");
	char s[16];
	Check(!TagWanted(0, 100.0f, s, sizeof s) && s[0] == '\0', "nothing at no stars");
	Check(TagWanted(3, 100.0f, s, sizeof s) && std::strcmp(s, "] 3") == 0, "three stars read \"] 3\"");
	Check(TagWanted(9, 100.0f, s, sizeof s) && std::strcmp(s, "] 6") == 0, "six at most");
	Check(!TagWanted(4, 0.0f, s, sizeof s), "a dead player is wanted by nobody on his tag");
	Check(TAG_WANTED_COLOR.r == 193 && TAG_WANTED_COLOR.g == 164 && TAG_WANTED_COLOR.b == 120,
	      "in the HUD's wanted colour");
}

void TestIslands() {
	std::printf("\nanother island: no ped, an arrow and a hint\n");
	Check(RemoteOnOtherIsland(LEVEL_COMMERCIAL, LEVEL_INDUSTRIAL), "Staunton is not Portland");
	Check(!RemoteOnOtherIsland(LEVEL_INDUSTRIAL, LEVEL_INDUSTRIAL), "Portland is Portland");
	Check(!RemoteOnOtherIsland(LEVEL_GENERIC, LEVEL_SUBURBAN) &&
	          !RemoteOnOtherIsland(LEVEL_SUBURBAN, LEVEL_GENERIC) &&
	          !RemoteOnOtherIsland(LEVEL_IGNORE, LEVEL_INDUSTRIAL),
	      "the generic zone on either side is nobody's island");
	char s[16];
	Check(TagIslandHint(LEVEL_SUBURBAN, s, sizeof s) && std::strcmp(s, "ON SHORESIDE") == 0,
	      "the hint names the island");
	Check(!TagIslandHint(LEVEL_GENERIC, s, sizeof s), "and has nothing to say over the water");
	for (const char *name : {IslandName(LEVEL_INDUSTRIAL), IslandName(LEVEL_COMMERCIAL),
	                         IslandName(LEVEL_SUBURBAN)}) {
		bool printable = name != nullptr;
		for (const char *c = name; printable && *c; ++c)
			printable = TagGlyph(*c) == *c;
		Check(printable, "an island's name is drawn as it is spelled");
	}
}

void TestDoor() {
	std::printf("\nthe safehouse door: save.sc's own angles\n");
	const HideoutDoorDef &door = HideoutDoor(DOOR_PORTLAND);
	Check(DoorTargetOpen(door, 210.0f) == 1 && DoorTargetOpen(door, 0.0f) == 0,
	      "210 opens it, 0 shuts it");
	Check(DoorTargetOpen(door, 90.0f) == -1, "anything else is not the door's business");
	Check(DoorHeadingAt(359.8f, 0.0f) && DoorHeadingAt(0.2f, 0.0f), "across the wrap");
	Check(!DoorHeadingAt(200.0f, 210.0f) && DoorHeadingAt(209.7f, 210.0f),
	      "a step short of open is not open");
	Check((GATE_MASK_KNOWN & GATE_BIT_HIDEOUT_DOOR) != 0 &&
	          (GATE_MASK_KNOWN & 0xFFu & ~GATE_BIT_HIDEOUT_DOOR) == (1u << GATE_COUNT) - 1,
	      "the door is the eighth bit and the gates keep theirs");
	Check(GATE_MASK_KNOWN == 0x3FF && sizeof(GateMaskBody) == 4,
	      "the other two doors are the next two bits, in the old pad");
	// The one SCM float the door's rotate has to carry, as fixed point.
	Check(static_cast<int16_t>(HIDEOUT_DOOR_OPEN * 16.0f) == 3360 &&
	          static_cast<int16_t>(HIDEOUT_DOOR_STEP * 16.0f) == 160,
	      "210 and 10 fit a script float");
}

void TestMoneyAndKills() {
	std::printf("\nthe scoreboard: money by the rule, kills and deaths\n");
	Check(!BoardShowsMoney(MONEY_RULE_OFF) && BoardShowsMoney(MONEY_RULE_OWN) &&
	          BoardShowsMoney(MONEY_RULE_SHARED),
	      "no MONEY column with money off");
	char s[24];
	MoneyLabel(s, sizeof s, true, 1250);
	Check(std::strcmp(s, "$1250") == 0, "$1250");
	MoneyLabel(s, sizeof s, true, -50);
	Check(std::strcmp(s, "-$50") == 0, "a fine past zero is -$50");
	MoneyLabel(s, sizeof s, false, 1250);
	Check(std::strcmp(s, "-") == 0, "somebody who has not said is a dash");
	MoneyLabel(s, sizeof s, true, 2000000000);
	Check(std::strcmp(s, "$99999999+") == 0, "a cheat's fortune stays in its column");
	MoneyLabel(s, sizeof s, true, INT32_MIN);
	Check(std::strcmp(s, "-$99999999+") == 0, "and so does the most negative");
	KillsLabel(s, sizeof s, 3, 1);
	Check(std::strcmp(s, "3/1") == 0, "3/1");

	Check(BOARD_STARS_X + 6 * BOARD_STAR_STEP <= BOARD_MONEY_X &&
	          BOARD_MONEY_X + BOARD_MONEY_W <= BOARD_KD_X &&
	          BOARD_KD_X + BOARD_KD_W <= BOARD_STATE_X &&
	          BOARD_STATE_X + BOARD_STATE_W < BOARD_PING_X,
	      "stars, money, K/D and status each have their own column");
	Check(BOARD_WIDTH <= BOARD_REF_WIDTH - 2.0f * BOARD_MARGIN_PX,
	      "the wider panel still fits the HUD's 640");
}

// ---- the retail bytes ---------------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE)
			return true;
	}
	return false;
}

bool BytesAt(const std::vector<uint8_t> &img, uintptr_t va, const uint8_t *want, size_t n) {
	return std::memcmp(&img[va - IMAGE_BASE], want, n) == 0;
}

uint32_t Dword(const std::vector<uint8_t> &img, uintptr_t va) {
	uint32_t v = 0;
	std::memcpy(&v, &img[va - IMAGE_BASE], 4);
	return v;
}

bool CallsAt(const std::vector<uint8_t> &img, uintptr_t site, uintptr_t target) {
	return img[site - IMAGE_BASE] == 0xE8 &&
	       site + 5 + static_cast<int32_t>(Dword(img, site + 1)) == target;
}

void TestAgainstTheImage() {
	std::printf("\nthe retail bytes under all of it\n");
	std::vector<uint8_t> img;
	if (!LoadExe(img)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE\n");
		return;
	}
	Check(BytesAt(img, WINMAIN_WAITMESSAGE_CALL, WAITMESSAGE_CALL_BYTES,
	              sizeof WAITMESSAGE_CALL_BYTES),
	      "0x00582F37 is WinMain's `call [0061D558h]`");
	static const uint8_t foreground[] = {0x83, 0x3D, 0x00, 0xF0, 0x60, 0x00, 0x00, 0x0F, 0x84};
	Check(BytesAt(img, 0x00582A50, foreground, sizeof foreground) &&
	          0x00582A57 + 6 + static_cast<int32_t>(Dword(img, 0x00582A59)) == 0x00582F06,
	      "and the loop reaches it only with ForegroundApp clear");
	Check(CallsAt(img, 0x0048E483, CTimer__Update) && CallsAt(img, 0x0048E49B, CGame__Process),
	      "Idle is CTimer::Update, then CGame::Process");
	Check(BytesAt(img, IDLE_NUMLIGHTS_RESET, IDLE_NUMLIGHTS_RESET_BYTES,
	              sizeof IDLE_NUMLIGHTS_RESET_BYTES) &&
	          BytesAt(img, ADDLIGHT_NUMLIGHTS_BOUND, ADDLIGHT_NUMLIGHTS_BOUND_BYTES,
	                  sizeof ADDLIGHT_NUMLIGHTS_BOUND_BYTES),
	      "NumLights is zeroed by Idle between the two, and bounded by AddLight");

	// The menu shut for the session's mission (pause.h).
	Check(BytesAt(img, CMenuManager__RequestFrontEndShutDown, REQUEST_FRONTEND_SHUTDOWN_BYTES,
	              sizeof REQUEST_FRONTEND_SHUTDOWN_BYTES) &&
	          CallsAt(img, 0x00488765, 0x0057CCF0) && img[0x0048876D - IMAGE_BASE] == 0xC3,
	      "RequestFrontEndShutDown raises the request, puts the music back and returns");
	static const uint8_t switchReads[] = {0x80, 0x3D, 0x6A, 0xCD, 0x95, 0x00, 0x01};
	Check(CallsAt(img, 0x0048516F, 0x00488790) && BytesAt(img, 0x004887F8, switchReads, sizeof switchReads),
	      "and Process's SwitchMenuOnAndOff is what acts on it");
	Check(BytesAt(img, MENU_PROCESS_SAVE_MENU_TEST, MENU_PROCESS_SAVE_MENU_TEST_BYTES,
	              sizeof MENU_PROCESS_SAVE_MENU_TEST_BYTES) &&
	          CallsAt(img, 0x0048511E, 0x0046B9C0),
	      "Process waits out a fade unless the save menu (+0x453) is up");

	// ROTATE_OBJECT, 034D: the 800 table (0x005EF77C) at 0x34D - 0x320.
	const uint32_t rotate = Dword(img, 0x005EF77C + (0x34D - 0x320) * 4);
	static const uint8_t collect4[] = {0x6A, 0x04};
	static const uint8_t heading[]  = {0xD9, 0x46, 0x14, 0xD9, 0xE0, 0xD9, 0x46, 0x18, 0xD9, 0xF3};
	Check(rotate == 0x00449893 && BytesAt(img, 0x0044989E, collect4, sizeof collect4) &&
	          CallsAt(img, 0x004498A1, CTheScripts__CollectParameters) &&
	          BytesAt(img, 0x004498BF, heading, sizeof heading),
	      "ROTATE_OBJECT takes four operands and turns by Atan2(-forward.x, forward.y)");

	static const uint8_t farclip[] = {0x80, 0x7C, 0x24, 0x38, 0x00, 0x75};
	Check(BytesAt(img, 0x0051C41C, farclip, sizeof farclip),
	      "CalcScreenCoors tests the far clip only when asked to");
}

} // namespace

int RunGapHudTests() {
	g_gapFailures = 0;
	TestAwayReason();
	TestWantedTag();
	TestIslands();
	TestDoor();
	TestMoneyAndKills();
	TestAgainstTheImage();
	return g_gapFailures;
}
