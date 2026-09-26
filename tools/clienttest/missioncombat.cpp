// The owner's mission against every player in it: client/src/game/missioncombat.h.
//
// The blast scope and the enemy sweep can't run here. What can is every
// decision they make, and - with a retail exe handed over - the objective and
// car-mission numbers, the two offsets and the handlers they run, read back
// out of it.

#include "game/missioncombat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_mcFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_mcFailures;
}

void TestWhichBlastCounts() {
	std::printf("a participant's blast and fire on the mission's only-the-player targets\n");
	Check(ParticipantBlastCounts(WEAPONTYPE_EXPLOSION), "the blast itself, as the engine names it");
	Check(ParticipantBlastCounts(WEAPONTYPE_ROCKETLAUNCHER) &&
	          ParticipantBlastCounts(WEAPONTYPE_GRENADE) &&
	          ParticipantBlastCounts(WEAPONTYPE_MOLOTOV),
	      "and the three weapons it comes out of");
	Check(ParticipantBlastCounts(WEAPONTYPE_FLAMETHROWER), "the fire it leaves behind");
	Check(!ParticipantBlastCounts(WEAPONTYPE_COLT45) && !ParticipantBlastCounts(WEAPONTYPE_M16) &&
	          !ParticipantBlastCounts(WEAPONTYPE_UZI_DRIVEBY),
	      "a round never: it arrives as a hit, under the scope that already counts it");
	Check(!ParticipantBlastCounts(WEAPONTYPE_RAMMEDBYCAR) &&
	          !ParticipantBlastCounts(WEAPONTYPE_RUNOVERBYCAR),
	      "nor a car, for the same reason");
	Check(!ParticipantBlastCounts(WEAPONTYPE_DROWNING) && !ParticipantBlastCounts(WEAPONTYPE_FALL),
	      "nor what the world does on its own");
}

float Sq(float m) { return m * m; }

void TestTheNearestIsChosen() {
	std::printf("an enemy with nobody goes for the nearest\n");
	const float distSq[4] = {Sq(40), Sq(12), Sq(30), Sq(5)};
	const bool  valid[4]  = {true, true, true, false};
	Check(ChooseEnemyTarget(-1, distSq, valid, 4, 0) == 1, "the nearest standing player");
	const bool none[4] = {false, false, false, false};
	Check(ChooseEnemyTarget(-1, distSq, none, 4, 0) == -1, "nobody when nobody stands");
	Check(ChooseEnemyTarget(3, distSq, valid, 4, 0) == 1,
	      "a target who is down is dropped at once, however recently chosen");
	Check(ChooseEnemyTarget(9, distSq, valid, 4, 99999) == 1, "and so is one out of range");
}

void TestItKeepsItsMan() {
	std::printf("an enemy with somebody keeps him, unless\n");
	float      distSq[3] = {Sq(60), Sq(20), 0};
	const bool valid[3]  = {true, true, false};
	Check(ChooseEnemyTarget(0, distSq, valid, 3, ENEMY_HOLD_MS - 1) == 0,
	      "not before the hold is up, however much nearer the other is");
	Check(ChooseEnemyTarget(0, distSq, valid, 3, ENEMY_HOLD_MS) == 1,
	      "after it, one a good deal nearer takes him over");
	distSq[0] = Sq(14);
	distSq[1] = Sq(1);
	Check(ChooseEnemyTarget(0, distSq, valid, 3, 60000) == 0,
	      "never in the thick of a fight, however near the other");
	distSq[0] = Sq(30);
	distSq[1] = Sq(19);
	Check(ChooseEnemyTarget(0, distSq, valid, 3, 60000) == 0,
	      "nor for one merely somewhat nearer (19 m against 30)");
	distSq[0] = Sq(100);
	distSq[1] = Sq(59);
	Check(ChooseEnemyTarget(0, distSq, valid, 3, 60000) == 1,
	      "far off, 59 m against 100 is enough");
	distSq[0] = Sq(20);
	distSq[1] = Sq(11);
	Check(ChooseEnemyTarget(0, distSq, valid, 3, 60000) == 0,
	      "near, 11 m against 20 is not: nine metres is not worth turning round for");

	// Two players circling each other past the enemy: it must not flip every
	// sweep. At 30 and 28 m, then 28 and 30, and so on, it keeps its first.
	int   at      = ChooseEnemyTarget(-1, distSq, valid, 3, 0);
	int   flips   = 0;
	for (int i = 0; i < 20; ++i) {
		distSq[0] = Sq(i % 2 ? 28.0f : 30.0f);
		distSq[1] = Sq(i % 2 ? 30.0f : 28.0f);
		const int next = ChooseEnemyTarget(at, distSq, valid, 3, 60000);
		flips += next != at;
		at = next;
	}
	Check(flips == 0, "two players trading places at the same distance never turn it");
}

void TestWhatItIsDoing() {
	std::printf("what a watched pedestrian is doing with its order\n");
	using mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT;
	Check(ClassifyEnemyPed(true, OBJECTIVE_KILL_CHAR_ON_FOOT, false, false, true) ==
	          EnemyHold::Engaged,
	      "on it, at a player");
	Check(ClassifyEnemyPed(true, OBJECTIVE_KILL_CHAR_ON_FOOT, false, true, false) ==
	          EnemyHold::Lost,
	      "its man gone (the engine nils a rebuilt replica out of m_pedInObjective)");
	Check(ClassifyEnemyPed(true, OBJECTIVE_KILL_CHAR_ON_FOOT, false, false, false) ==
	          EnemyHold::Foreign,
	      "the same objective at a pedestrian is the script's own order");
	Check(ClassifyEnemyPed(false, 0, false, false, false) == EnemyHold::Lost,
	      "no objective at all is one the engine completed and restored");
	Check(ClassifyEnemyPed(false, 13, true, false, false) == EnemyHold::Busy,
	      "out of a car on the way to it, the order stored behind: left alone");
	Check(ClassifyEnemyPed(false, 2, false, false, false) == EnemyHold::Foreign,
	      "told to flee: the mission's, not the sweep's");

	std::printf("what a watched rammer is doing\n");
	Check(ClassifyRammer(mcombat::CARMISSION_RAMPLAYER_FARAWAY, false) == EnemyHold::Engaged &&
	          ClassifyRammer(mcombat::CARMISSION_RAMPLAYER_CLOSE, false) == EnemyHold::Engaged,
	      "ramming our player, far or close");
	Check(ClassifyRammer(mcombat::CARMISSION_RAMCAR_CLOSE, false) == EnemyHold::Engaged,
	      "ramming a participant's car");
	Check(ClassifyRammer(mcombat::CARMISSION_RAMCAR_FARAWAY, true) == EnemyHold::Lost,
	      "whose car is gone");
	Check(ClassifyRammer(mcombat::CARMISSION_NONE, false) == EnemyHold::Lost,
	      "and after the AI gave up on it for that");
	Check(ClassifyRammer(8, false) == EnemyHold::Foreign && ClassifyRammer(1, false) == EnemyHold::Foreign,
	      "driving somewhere, or cruising: the script's plans");
}

// ---- against the real exe ---------------------------------------------------

constexpr uint32_t IMAGE_BASE = 0x00400000;
constexpr size_t   IMAGE_SIZE = 2383872;

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
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
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool CallsAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t target) {
	return img[site - IMAGE_BASE] == 0xE8 && site + 5 + Dword(img, site + 1) == target;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> b) {
	size_t i = 0;
	for (uint8_t x : b)
		if (img[va - IMAGE_BASE + i++] != x)
			return false;
	return true;
}

void TestAgainstTheImage() {
	std::printf("\nthe enemies' orders against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "enemies' orders against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());
	using namespace mcombat;

	auto sameArm = [&](uint32_t objective) {
		return Dword(img, SET_OBJECTIVE_SAME_TABLE + (objective - 6) * 4);
	};
	Check(Bytes(img, 0x004D8432, {0x8D, 0x46, 0xFA}) && Bytes(img, 0x004D843C, {0xFF, 0x24, 0x85}) &&
	          Dword(img, 0x004D843F) == SET_OBJECTIVE_SAME_TABLE,
	      "SetObjective, given the objective the ped holds, jumps by objective - 6");
	Check(Bytes(img, SET_OBJECTIVE_PED_ARM, {0x39, 0xAB}) &&
	          Dword(img, SET_OBJECTIVE_PED_ARM + 2) == PED_IN_OBJECTIVE &&
	          Bytes(img, SET_OBJECTIVE_CAR_ARM, {0x39, 0xAB}) &&
	          Dword(img, SET_OBJECTIVE_CAR_ARM + 2) == offs::PED_CAR_IN_OBJECTIVE,
	      "to an arm comparing m_pedInObjective (+16Ch) or m_carInObjective (+170h)");
	Check(sameArm(OBJECTIVE_KILL_CHAR_ON_FOOT) == SET_OBJECTIVE_PED_ARM &&
	          sameArm(OBJECTIVE_KILL_CHAR_ANY_MEANS) == SET_OBJECTIVE_PED_ARM,
	      "the two kill objectives, 7 and 8, compare the ped");
	Check(sameArm(OBJECTIVE_DESTROY_CAR) == SET_OBJECTIVE_CAR_ARM &&
	          sameArm(OBJECTIVE_ENTER_CAR_AS_DRIVER) == SET_OBJECTIVE_CAR_ARM,
	      "DESTROY_CAR, 19, the car, as the enter-car objectives do");
	Check(Bytes(img, 0x004D8403, {0x8B, 0x83}) && Dword(img, 0x004D8405) == offs::PED_PREV_OBJECTIVE &&
	          Bytes(img, 0x004D8409, {0x39, 0xF0, 0x75, 0x0A, 0x85, 0xC0, 0x74, 0x06}),
	      "and it refuses an objective equal to the stored one, before anything");

	Check(Bytes(img, KILL_CHAR_ON_FOOT_HANDLER + 0x43, {0x6A, 0x07}) &&
	          CallsAt(img, KILL_CHAR_ON_FOOT_HANDLER + 0x45, 0x004D83E0),
	      "01C9 sets objective 7");
	Check(Bytes(img, DESTROY_CAR_HANDLER + 0x3D, {0x6A, 0x13}) &&
	          CallsAt(img, DESTROY_CAR_HANDLER + 0x45, 0x004D83E0) &&
	          CallsAt(img, DESTROY_CAR_HANDLER + 0x2C, 0x0043EAF0),
	      "01D9 sets objective 19 on the car it looks up in the vehicle pool");
	Check(Dword(img, g_ScriptOpcodeTable_400 + (OP_SET_CHAR_OBJ_DESTROY_CAR - 400) * 4) ==
	              DESTROY_CAR_HANDLER &&
	          Dword(img, g_ScriptOpcodeTable_400 + (0x01C9 - 400) * 4) == KILL_CHAR_ON_FOOT_HANDLER,
	      "and both are the 400 table's own");

	Check(Dword(img, g_ScriptOpcodeTable_100 + (OP_SET_CAR_MISSION - 100) * 4) ==
	              SET_CAR_MISSION_HANDLER &&
	          Bytes(img, SET_CAR_MISSION_HANDLER + 0x24, {0x88, 0x88}) &&
	          Dword(img, SET_CAR_MISSION_HANDLER + 0x26) == offs::AUTOPILOT_CAR_MISSION,
	      "00AF writes its operand into m_nCarMission as it stands");
	Check(Dword(img, g_ScriptOpcodeTable_800 + (OP_SET_CAR_RAM_CAR - 800) * 4) ==
	              SET_CAR_RAM_CAR_HANDLER &&
	          CallsAt(img, SET_CAR_RAM_CAR_HANDLER + 0x39, CCarAI__TellCarToRamOtherCar),
	      "032C hands both cars to CCarAI::TellCarToRamOtherCar");
	Check(Bytes(img, CCarAI__TellCarToRamOtherCar + 9, {0x89, 0x8B}) &&
	          Dword(img, CCarAI__TellCarToRamOtherCar + 11) == AUTOPILOT_TARGET_CAR &&
	          Bytes(img, CCarAI__TellCarToRamOtherCar + 0x1B, {0xC6, 0x83}) &&
	          Dword(img, CCarAI__TellCarToRamOtherCar + 0x1D) == offs::AUTOPILOT_CAR_MISSION &&
	          img[CCarAI__TellCarToRamOtherCar + 0x21 - IMAGE_BASE] == CARMISSION_RAMCAR_FARAWAY,
	      "which puts the target in m_pTargetCar (+198h) and RAMCAR_FARAWAY, 15, in the mission");
	Check(Bytes(img, RAM_COLLISION_TEST + 0x0B, {0x80, 0xBD}) &&
	          img[RAM_COLLISION_TEST + 0x11 - IMAGE_BASE] == CARMISSION_RAMPLAYER_CLOSE &&
	          Bytes(img, RAM_COLLISION_TEST + 0x31, {0x80, 0xBD}) &&
	          img[RAM_COLLISION_TEST + 0x37 - IMAGE_BASE] == CARMISSION_RAMCAR_CLOSE &&
	          Dword(img, RAM_COLLISION_TEST + 0x3C) == AUTOPILOT_TARGET_CAR,
	      "and a rammer's collision test knows RAMPLAYER_CLOSE as 3 and RAMCAR_CLOSE as 16");
	Check(Bytes(img, 0x004EA4C0, {0x8A, 0x45, 0x53, 0xC0, 0xE8, 0x04}) &&
	          Bytes(img, 0x004EA4E3, {0x83, 0x7C, 0x24, 0x38, WEAPONTYPE_EXPLOSION}),
	      "CPed::InflictDamage's only-the-player test waives the blast, and nothing else we send");
}

} // namespace

int RunMissionCombatTests() {
	g_mcFailures = 0;
	TestWhichBlastCounts();
	TestTheNearestIsChosen();
	TestItKeepsItsMan();
	TestWhatItIsDoing();
	TestAgainstTheImage();
	return g_mcFailures;
}
