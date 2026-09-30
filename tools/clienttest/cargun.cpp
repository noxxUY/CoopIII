// The tank's cannon and the fire truck's water cannon are the driver's
// (client/src/game/cargun.h).
//
// The two taken calls can't run here. What can is whose pad a gun answers to,
// the fire truck rule that stands behind it, and, with a retail exe handed
// over, the two call sites and everything the redirection relies on around
// them, read back out of it.

#include "game/addresses.h"
#include "game/cargun.h"
#include "game/emergency.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_carGunFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_carGunFailures;
}

void TestWhosePad() {
	std::printf("\nwhose pad works the gun of the car we sit in\n");
	int  tank = 0, us = 0, driver = 0;
	void *const car = &tank;
	Check(CarGunForPad(car, &us, &us) == car, "at its wheel, the gun is ours, as in single player");
	Check(CarGunForPad(car, &driver, &us) == nullptr,
	      "in a passenger seat of somebody's car, it is not: no turn, no shell, no jet");
	Check(CarGunForPad(car, nullptr, &us) == nullptr,
	      "nor in a passenger seat of a car nobody drives");
	Check(CarGunForPad(nullptr, nullptr, &us) == nullptr && CarGunForPad(nullptr, &us, &us) == nullptr,
	      "on foot there is no car to give");
	Check(CarGunForPad(car, nullptr, nullptr) == nullptr,
	      "and with no player ped, nobody is at the wheel");
}

void TestThePassengersJet() {
	std::printf("\na passenger's fire truck jet, behind the gate\n");
	// A passenger is never the local driver. In a truck another machine moves
	// his jet is refused and the driver's comes off the wire; in one this
	// machine moves, or the session has no name for, the truck's NPC arm is
	// what sprays, as it would with nobody aboard.
	Check(!CannonInputMayRun(false, true, true),
	      "his jet from a truck somebody else drives is refused");
	Check(CannonInputMayRun(false, true, false) && CannonInputMayRun(false, false, false),
	      "the NPC arm's jet from a truck this machine moves still runs");
	Check(CannonInputMayRun(true, true, false) && CannonInputMayRun(true, false, false),
	      "and the driver's own always does");
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

std::vector<uint32_t> CallersOf(const std::vector<uint8_t> &img, uint32_t target) {
	std::vector<uint32_t> out;
	const uint32_t first = 0x00401000, last = 0x005E3000;
	for (uint32_t va = first; va + 5 <= last; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && va + 5 + Dword(img, va + 1) == target)
			out.push_back(va);
	return out;
}

bool AnyCallTo(const std::vector<uint8_t> &img, uint32_t from, uint32_t to, uint32_t target) {
	for (uint32_t va = from; va + 5 <= to; ++va)
		if (CallsAt(img, va, target))
			return true;
	return false;
}

// `call FindPlayerVehicle / cmp ebx,eax / jne <out>`.
bool GateAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t out) {
	return CallsAt(img, site, FindPlayerVehicle) &&
	       Bytes(img, site + 5, {0x39, 0xC3, 0x0F, 0x85}) &&
	       site + 13 + Dword(img, site + 9) == out;
}

void TestAgainstTheImage() {
	std::printf("\nthe two gun gates, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "gun gates against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, FindPlayerVehicle,
	            {0x0F, 0xB6, 0x05, 0x61, 0xCD, 0x95, 0x00, 0x6B, 0xC0, 0x4F, 0x8B, 0x0C, 0x85,
	             0xF0, 0x12, 0x94, 0x00, 0x85, 0xC9, 0x74, 0x10, 0x80, 0xB9, 0x14, 0x03, 0x00,
	             0x00, 0x00, 0x74, 0x07, 0x8B, 0x81, 0x10, 0x03, 0x00, 0x00, 0xC3, 0x31, 0xC0,
	             0xC3}),
	      "FindPlayerVehicle is m_pMyVehicle whenever bInVehicle is set, any seat; no "
	      "arguments, plain ret");

	Check(Bytes(img, CAutomobile__FireTruckControl,
	            {0x53, 0x56, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x89, 0xCB}) &&
	          FIRE_TRUCK_PLAYER_CAR_CALL == CAutomobile__FireTruckControl + 0x0A,
	      "FireTruckControl takes `this` into ebx and asks at once, at 0x0052259A");
	Check(GateAt(img, FIRE_TRUCK_PLAYER_CAR_CALL, FIRE_TRUCK_AI_ARM),
	      "call FindPlayerVehicle / cmp ebx,eax / jne to the NPC arm");
	Check(Bytes(img, FIRE_TRUCK_AI_ARM,
	            {0x8A, 0x4B, 0x50, 0xC0, 0xE9, 0x03, 0x0F, 0xB6, 0xC1, 0x83, 0xF8, 0x03}),
	      "and the NPC arm sprays only for a truck in STATUS_PHYSICS");
	Check(CallsAt(img, FIRE_TRUCK_CANNON_CALL, CWaterCannons__UpdateOne) &&
	          FIRE_TRUCK_CANNON_CALL > FIRE_TRUCK_AI_ARM,
	      "whose jet still reaches the call emergency.cpp takes");

	Check(Bytes(img, CAutomobile__TankControl,
	            {0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xC8, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x0053D55B, {0x89, 0xCB}),
	      "TankControl pushes four, takes 1C8h of stack, and `this` into ebx");
	Check(GateAt(img, TANK_PLAYER_CAR_CALL, TANK_CONTROL_EPILOGUE),
	      "call FindPlayerVehicle / cmp ebx,eax / jne to the end, at 0x0053D5E5");
	Check(Bytes(img, TANK_CONTROL_EPILOGUE,
	            {0x81, 0xC4, 0xC8, 0x01, 0x00, 0x00, 0x5D, 0x5F, 0x5E, 0x5B, 0xC3}),
	      "and the end undoes exactly that prologue and returns");

	Check(!AnyCallTo(img, CAutomobile__FireTruckControl, FIRE_TRUCK_PLAYER_CAR_CALL,
	                 CPad__GetPad) &&
	          !AnyCallTo(img, CAutomobile__TankControl, TANK_PLAYER_CAR_CALL, CPad__GetPad),
	      "nothing in either reads the pad before its gate");
	Check(FIRE_TRUCK_PLAYER_CAR_CALL + 5 <= 0x005225A9 && CallsAt(img, 0x005225A9, CPad__GetPad) &&
	          CallsAt(img, 0x005225D2, 0x004930C0) && CallsAt(img, 0x0052260E, 0x00493070),
	      "the fire truck's fire button and stick come after it");
	Check(CallsAt(img, 0x0053D628, 0x004930C0) && CallsAt(img, 0x0053D6ED, 0x004934F0) &&
	          CallsAt(img, 0x0053DA3C, CExplosion__AddExplosion),
	      "and so do the tank's stick, its fire button and its shell");

	const std::vector<uint32_t> truck = CallersOf(img, CAutomobile__FireTruckControl);
	const std::vector<uint32_t> tank  = CallersOf(img, CAutomobile__TankControl);
	Check(truck.size() == 1 && truck[0] == 0x00531FF7 && tank.size() == 1 && tank[0] == 0x0053200A,
	      "each has one caller, CAutomobile::ProcessControl's model switch");

	// Neither site is on five bytes anybody else writes.
	const uint32_t ours[]   = {FIRE_TRUCK_PLAYER_CAR_CALL, TANK_PLAYER_CAR_CALL};
	const uint32_t others[] = {0x005225D2, 0x0052260E, 0x0053D628, CCam__WellBufferMe,
	                           CCam__Process_CamOnAStringCall, CCam__Process_BehindBoatCall,
	                           0x0048BFB0, FIRE_TRUCK_CANNON_CALL, TANK_TURRET_SOUND_CALL,
	                           CAutomobile__FireTruckControl, CAutomobile__TankControl};
	bool apart = true;
	for (uint32_t a : ours)
		for (uint32_t b : others)
			if (a < b + 5 && b < a + 5)
				apart = false;
	Check(apart, "neither gate overlaps SACarCam's sites, our other call sites, or either "
	             "function's first five bytes");
}

} // namespace

int RunCarGunTests() {
	g_carGunFailures = 0;
	TestWhosePad();
	TestThePassengersJet();
	TestAgainstTheImage();
	return g_carGunFailures;
}
