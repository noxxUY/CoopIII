// A stunt bonus for a car somebody else flies: client/src/game/stunt.h.
//
// The redirect can't run here. What can is the rule it applies over the same
// ClassifyCar the damage detours use, and - with a retail exe handed over -
// the instruction it sits in, read back out of it.

#include "game/stunt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_stuntFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_stuntFailures;
}

void TestWhoseAirCounts() {
	std::printf("whose car may be in the air\n");
	Check(AirborneCounts(true, CarOwner::Local), "a car this engine moves, off the ground, is");
	Check(!AirborneCounts(false, CarOwner::Local), "and on the ground it isn't");
	Check(!AirborneCounts(true, CarOwner::RemoteDriver),
	      "a teammate's car we ride in is never in the air here");
	Check(!AirborneCounts(true, CarOwner::RemoteCustodian),
	      "nor one a teammate is settling");
	Check(!AirborneCounts(true, CarOwner::RemoteHost), "nor another machine's traffic");
	Check(!AirborneCounts(true, CarOwner::Nobody),
	      "nor a session car nobody holds, which every machine parks and skips");
}

void TestTheSeatsThatMatter() {
	std::printf("the seats the stunt threads ask about\n");
	// ClassifyCar(authorised, weDrive, remoteDriver, remoteCustodian, replica, unheld)
	Check(!AirborneCounts(true, ClassifyCar(false, false, true, false, false)),
	      "a passenger in the car another player drives");
	Check(!AirborneCounts(true, ClassifyCar(false, false, false, false, false, true)),
	      "a passenger left in it after the driver got out");
	Check(AirborneCounts(true, ClassifyCar(false, true, true, false, false)),
	      "our own wheel wins over a session that still names the last driver");
	Check(AirborneCounts(true, ClassifyCar(false, true, false, false, false, true)),
	      "and over a session car nobody else holds");
	Check(AirborneCounts(true, ClassifyCar(false, false, false, false, false)),
	      "a car nobody else has a claim on is ours, as in single player");
}

// A unique jump pays its driver: the $5000, the stat and the insane stunt
// bonus are his machine's, whichever machine moves the car.
void TestARiderNeverJumps() {
	std::printf("the jump is the driver's\n");
	Check(!AirborneCounts(true, CarOwner::Local, true),
	      "a rider in a car this engine moves is not in the air to his own stunt threads");
	Check(!AirborneCounts(true, ClassifyCar(false, false, false, false, false), true),
	      "nor in one nobody else has a claim on, a script's driver at the wheel");
	Check(!AirborneCounts(true, ClassifyCar(false, false, true, false, false), true),
	      "nor, still, in a teammate's");
	Check(AirborneCounts(true, ClassifyCar(false, true, false, false, false), false),
	      "and the driver's real jump still counts");
}

// ---- against the real exe ---------------------------------------------------

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
	std::printf("\nIS_CAR_IN_AIR_PROPER against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "stunt answer against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, CRunningScript__ProcessCommands400To499 + 0x11,
	            {0x8D, 0x90, 0x70, 0xFE, 0xFF, 0xFF}) &&
	          Bytes(img, CRunningScript__ProcessCommands400To499 + 0x19, {0x83, 0xFA, 0x63}) &&
	          Bytes(img, CRunningScript__ProcessCommands400To499 + 0x24, {0xFF, 0x24, 0x95}) &&
	          Dword(img, CRunningScript__ProcessCommands400To499 + 0x27) == g_ScriptOpcodeTable_400,
	      "the 400 range rebases by 190h and jumps through its table");
	Check(Dword(img, g_ScriptOpcodeTable_400 + (OPCODE_IS_CAR_IN_AIR_PROPER - 400) * 4) ==
	          IS_CAR_IN_AIR_PROPER_HANDLER,
	      "01F3 is entry 99, the handler at 0x004428F0");
	int into = 0;
	for (uint32_t i = 0; i < 100; ++i) {
		const uint32_t to = Dword(img, g_ScriptOpcodeTable_400 + i * 4);
		if (to >= IS_CAR_IN_AIR_PROPER_HANDLER && to <= IS_CAR_IN_AIR_PROPER_ANSWER)
			++into;
	}
	Check(into == 1, "and no other entry lands in it");
	Check(CallsAt(img, 0x004428F8, CTheScripts__CollectParameters),
	      "it collects the one operand");
	Check(Bytes(img, 0x00442902, {0x8B, 0x0D}) && Dword(img, 0x00442904) == CPools__ms_pVehiclePool &&
	          CallsAt(img, 0x00442909, CPool_CVehicle__GetAt),
	      "looks the car up in the vehicle pool");
	Check(Bytes(img, 0x0044290E, {0x80, 0xB8}) &&
	          Dword(img, 0x00442910) == offs::PHYSICAL_COLLISION_RECORDS &&
	          img[0x00442914 - IMAGE_BASE] == 0,
	      "and calls it airborne when its collision record count is 0");
	Check(CallsAt(img, IS_CAR_IN_AIR_PROPER_ANSWER, CRunningScript__UpdateCompareFlag),
	      "whose answer goes to UpdateCompareFlag at 0x00442925");
	Check(Bytes(img, CRunningScript__UpdateCompareFlag, {0x80, 0xB9}) &&
	          Dword(img, CRunningScript__UpdateCompareFlag + 2) == offs::SCRIPT_NOT &&
	          Bytes(img, CRunningScript__UpdateCompareFlag + 7, {0x8B, 0x44, 0x24, 0x04}),
	      "which applies the NOT itself to the byte it is handed on the stack");
	Check(Bytes(img, 0x0053298E, {0xC6, 0x85}) &&
	          Dword(img, 0x00532990) == offs::PHYSICAL_COLLISION_RECORDS &&
	          img[0x00532994 - IMAGE_BASE] == 0,
	      "CAutomobile::ProcessControl's skip arm zeroes the count");
	Check(Bytes(img, 0x00497224, {0xFE, 0x85}) &&
	          Dword(img, 0x00497226) == offs::PHYSICAL_COLLISION_RECORDS,
	      "and AddCollisionRecord is what counts it up");
}

} // namespace

int RunStuntTests() {
	g_stuntFailures = 0;
	TestWhoseAirCounts();
	TestTheSeatsThatMatter();
	TestARiderNeverJumps();
	TestAgainstTheImage();
	return g_stuntFailures;
}
