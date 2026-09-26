// A car's end: client/src/game/wreck.h.
//
// The decisions are covered in main.cpp, where the roster runs them. What is
// here is what they rest on in the retail image, read back out of it when one
// is handed over: the writes a joiner's shell copies from BlowUpCar, the
// culprit that says whose blast it was, and the fire timer's own BlowUpCar.

#include "game/wreck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_wreckFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_wreckFailures;
}

void TestTheShellsPlan() {
	std::printf("\nthe shell a joiner is handed\n");
	const QuietWreckPlan car = PlanQuietWreck(VEHICLE_TYPE_CAR, 90);
	Check(car.automobile && car.bodywork, "a car gets all of it");
	const QuietWreckPlan bandit = PlanQuietWreck(VEHICLE_TYPE_CAR, MI_RCBANDIT);
	Check(bandit.automobile && !bandit.bodywork, "the RC Bandit keeps its doors, as in BlowUpCar");
	const QuietWreckPlan boat = PlanQuietWreck(VEHICLE_TYPE_BOAT, 90);
	Check(!boat.automobile && !boat.bodywork,
	      "a boat gets only what CBoat::BlowUpCar writes: no time of death, no damage model");

	bool doors = true;
	for (size_t i = 0; i < 6; ++i)
		for (size_t j = i + 1; j < 6; ++j)
			if (WRECK_DOORS[i].part == WRECK_DOORS[j].part ||
			    WRECK_DOORS[i].component == WRECK_DOORS[j].component)
				doors = false;
	Check(doors, "six different doors on six different nodes");
	Check(WRECK_BUMPERS[0].part == VEHBUMPER_FRONT && WRECK_BUMPERS[1].part == VEHBUMPER_REAR,
	      "and both bumpers");
}

// ---- against the real exe ------------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t bits = Dword(img, va);
	float          f    = 0.0f;
	std::memcpy(&f, &bits, sizeof f);
	return f;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> want) {
	uint32_t at = va;
	for (uint8_t b : want)
		if (Byte(img, at++) != b)
			return false;
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

// `push 0 / push part / push component / call` ending at `site`: BlowUpCar's
// shape for all eight of its damage calls.
bool DamageCall(const std::vector<uint8_t> &img, uint32_t site, uint32_t to,
                const WreckPart &p) {
	return Bytes(img, site - 8, {0x89, 0xD9, 0x6A, 0x00, 0x6A, p.part, 0x6A, p.component}) &&
	       CallsTo(img, site, to);
}

void TestTheShellAgainstTheImage() {
	std::printf("\na wreck without its blast, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "transcription against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, CAutomobile__BlowUpCar, {0x53, 0x56, 0x89, 0xCB, 0x57, 0x55}) &&
	          Bytes(img, CBoat__BlowUpCar, {0x53, 0x56, 0x57, 0x55, 0x89, 0xCD}),
	      "both BlowUpCars open where addresses.h says, ebx and ebp the car");

	// CAutomobile::BlowUpCar, in its order.
	const uint8_t wrecked = uint8_t(ENTITY_STATUS_WRECKED << ENTITY_STATUS_SHIFT);
	Check(Bytes(img, BLOWUP_CAR_STATUS,
	            {0x8A, 0x43, uint8_t(offs::ENTITY_FLAGS), 0x24, 0x07, 0x0C, wrecked, 0x88, 0x43,
	             uint8_t(offs::ENTITY_FLAGS)}),
	      "STATUS_WRECKED into bits 3-7 of +0x50, the type kept");
	Check(Bytes(img, BLOWUP_CAR_SCORCHED,
	            {0x8A, 0x43, uint8_t(offs::ENTITY_FLAGS_B), 0x24,
	             uint8_t(~offs::ENTITY_RENDER_SCORCHED), 0x0C, offs::ENTITY_RENDER_SCORCHED}),
	      "bRenderScorched");
	Check(Byte(img, BLOWUP_CAR_DEATH_TIME) == 0xA1 &&
	          Dword(img, BLOWUP_CAR_DEATH_TIME + 1) == CTimer__m_snTimeInMilliseconds &&
	          Bytes(img, BLOWUP_CAR_DEATH_TIME + 5, {0x89, 0x83}) &&
	          Dword(img, BLOWUP_CAR_DEATH_TIME + 7) == offs::VEH_TIME_OF_DEATH,
	      "m_nTimeOfDeath from CTimer::m_snTimeInMilliseconds");
	Check(Bytes(img, BLOWUP_CAR_FUCK_CALL - 6, {0x8D, 0x8B}) &&
	          Dword(img, BLOWUP_CAR_FUCK_CALL - 4) == offs::AUTO_DAMAGE_MANAGER &&
	          CallsTo(img, BLOWUP_CAR_FUCK_CALL, CDamageManager__FuckCarCompletely),
	      "FuckCarCompletely on the car's own CDamageManager");
	Check(Bytes(img, BLOWUP_CAR_RCBANDIT, {0x66, 0x81, 0x7B, uint8_t(offs::MODEL_INDEX),
	                                       uint8_t(MI_RCBANDIT), 0x00}),
	      "the bodywork is skipped for the RC Bandit");
	bool bumpers = true;
	for (size_t i = 0; i < 2; ++i)
		bumpers = bumpers && DamageCall(img, BLOWUP_CAR_BUMPER_CALLS[i],
		                                CAutomobile__SetBumperDamage, WRECK_BUMPERS[i]);
	Check(bumpers, "the two bumpers, in wreck.h's order, flying parts on");
	bool doors = true;
	for (size_t i = 0; i < 6; ++i)
		doors = doors && DamageCall(img, BLOWUP_CAR_DOOR_CALLS[i], CAutomobile__SetDoorDamage,
		                            WRECK_DOORS[i]);
	Check(doors, "and the six doors");
	Check(CallsTo(img, BLOWUP_CAR_DOOR_CALLS[5] + 11, CAutomobile__SpawnFlyingComponent) &&
	          Bytes(img, BLOWUP_CAR_DOOR_CALLS[5] + 7, {0x6A, 0x01, 0x6A, CAR_WHEEL_LF}),
	      "the front left wheel thrown, which the shell does not do");
	Check(Bytes(img, BLOWUP_CAR_WHEEL_NODE, {0x8B, 0x83}) &&
	          Dword(img, BLOWUP_CAR_WHEEL_NODE + 2) ==
	              offs::AUTO_CAR_NODES + 4u * uint32_t(CAR_WHEEL_LF) &&
	          Byte(img, BLOWUP_CAR_WHEEL_NODE + 6) == 0x68 &&
	          Dword(img, BLOWUP_CAR_WHEEL_NODE + 7) == GetFirstObjectCallback &&
	          CallsTo(img, BLOWUP_CAR_WHEEL_WALK, RwFrameForAllObjects) &&
	          Bytes(img, BLOWUP_CAR_WHEEL_WALK + 5, {0x83, 0xC4, 0x0C}) &&
	          Bytes(img, BLOWUP_CAR_WHEEL_HIDE, {0xC6, 0x40, uint8_t(RWOBJECT_FLAGS), 0x00}),
	      "and the atomic left on its node hidden, found by GetFirstObjectCallback");
	Check(Bytes(img, GetFirstObjectCallback,
	            {0x8B, 0x4C, 0x24, 0x04, 0x0F, 0xB6, 0x41, uint8_t(RWOBJECT_FLAGS), 0x83, 0xE0,
	             RPATOMIC_RENDER, 0x74, 0x06, 0x8B, 0x44, 0x24, 0x08, 0x89, 0x08, 0x89, 0xC8, 0xC3}),
	      "which keeps the frame's rendered atomic in *out");
	Check(CallsTo(img, 0x00530107, RwFrameForAllObjects) &&
	          Bytes(img, 0x0053010C, {0x83, 0xC4, 0x0C}),
	      "RwFrameForAllObjects is cdecl with three arguments at its other call too");
	Check(Bytes(img, BLOWUP_CAR_HEALTH, {0xC7, 0x83}) &&
	          Dword(img, BLOWUP_CAR_HEALTH + 2) == offs::VEH_HEALTH &&
	          Dword(img, BLOWUP_CAR_HEALTH + 6) == 0,
	      "m_fHealth = 0");
	Check(Bytes(img, BLOWUP_CAR_BOMB_TIMER, {0x66, 0xC7, 0x83}) &&
	          Dword(img, BLOWUP_CAR_BOMB_TIMER + 3) == offs::VEH_BOMB_TIMER &&
	          Bytes(img, BLOWUP_CAR_BOMB_TIMER + 7, {0x00, 0x00}),
	      "m_nBombTimer = 0");
	Check(Bytes(img, BLOWUP_CAR_BOMB_TYPE - 11, {0x8A, 0x83}) &&
	          Dword(img, BLOWUP_CAR_BOMB_TYPE - 9) == offs::AUTOMOBILE_BOMB &&
	          Bytes(img, BLOWUP_CAR_BOMB_TYPE, {0x24, uint8_t(~offs::AUTOMOBILE_BOMB_MASK)}),
	      "no bomb");
	const auto flagsAnd = [&](uint32_t at, uint32_t field, uint8_t bit) {
		return Bytes(img, at - 6, {0x8A, 0x83}) && Dword(img, at - 4) == field &&
		       Bytes(img, at, {0x24, uint8_t(~bit)});
	};
	Check(flagsAnd(BLOWUP_CAR_ENGINE, offs::VEH_FLAGS_A, offs::VEH_ENGINE_ON) &&
	          flagsAnd(BLOWUP_CAR_LIGHTS, offs::VEH_FLAGS_A, offs::VEH_LIGHTS_ON),
	      "engine and lights off");
	Check(Bytes(img, BLOWUP_CAR_SIREN, {0xC6, 0x83}) &&
	          Dword(img, BLOWUP_CAR_SIREN + 2) == offs::VEH_SIREN_OR_ALARM &&
	          Byte(img, BLOWUP_CAR_SIREN + 6) == 0,
	      "siren off");
	Check(flagsAnd(BLOWUP_CAR_TAXI, offs::AUTOMOBILE_BOMB, offs::AUTOMOBILE_TAXI_LIGHT),
	      "taxi light off");
	Check(Bytes(img, BLOWUP_CAR_LAW_CALL - 2, {0x6A, 0x00}) &&
	          CallsTo(img, BLOWUP_CAR_LAW_CALL, CVehicle__ChangeLawEnforcerState),
	      "and no longer a police car");
	Check(Bytes(img, CVehicle__ChangeLawEnforcerState, {0x80, 0x7C, 0x24, 0x04, 0x00}) &&
	          Bytes(img, CVehicle__ChangeLawEnforcerState + 0x4E, {0xC2, 0x04, 0x00}) &&
	          Bytes(img, CVehicle__ChangeLawEnforcerState + 0x0D, {0x24, 0x01, 0x75}) &&
	          Bytes(img, CVehicle__ChangeLawEnforcerState + 0x36, {0x24, 0x01, 0x74}),
	      "ChangeLawEnforcerState takes one byte and moves nothing unless the bit changes");

	// CBoat::BlowUpCar.
	Check(Bytes(img, BLOWUP_BOAT_STATUS,
	            {0x8A, 0x45, uint8_t(offs::ENTITY_FLAGS), 0x24, 0x07, 0x0C, wrecked, 0x88, 0x45,
	             uint8_t(offs::ENTITY_FLAGS)}) &&
	          Bytes(img, BLOWUP_BOAT_SCORCHED,
	                {0x8A, 0x45, uint8_t(offs::ENTITY_FLAGS_B), 0x24,
	                 uint8_t(~offs::ENTITY_RENDER_SCORCHED), 0x0C, offs::ENTITY_RENDER_SCORCHED}),
	      "a boat: wrecked and scorched the same way");
	Check(Bytes(img, BLOWUP_BOAT_HEALTH, {0xC7, 0x85}) &&
	          Dword(img, BLOWUP_BOAT_HEALTH + 2) == offs::VEH_HEALTH &&
	          Dword(img, BLOWUP_BOAT_HEALTH + 6) == 0 &&
	          Bytes(img, BLOWUP_BOAT_BOMB_TIMER, {0x66, 0xC7, 0x85}) &&
	          Dword(img, BLOWUP_BOAT_BOMB_TIMER + 3) == offs::VEH_BOMB_TIMER,
	      "no health and no bomb timer");
	Check(Bytes(img, BLOWUP_BOAT_ENGINE, {0x24, uint8_t(~offs::VEH_ENGINE_ON)}) &&
	          Bytes(img, BLOWUP_BOAT_LIGHTS, {0x24, uint8_t(~offs::VEH_LIGHTS_ON)}) &&
	          Bytes(img, BLOWUP_BOAT_LAW_CALL - 8, {0x6A, 0x00}) &&
	          CallsTo(img, BLOWUP_BOAT_LAW_CALL, CVehicle__ChangeLawEnforcerState),
	      "engine and lights off, and no longer a police boat");
	bool boatTime = true;
	for (uint32_t va = CBoat__BlowUpCar; va < BLOWUP_BOAT_LAW_CALL; ++va)
		if (Dword(img, va) == offs::VEH_TIME_OF_DEATH && Byte(img, va - 1) == 0x85)
			boatTime = false;
	Check(boatTime, "and no time of death written before the explosion, which is why a "
	                "boat's shell has none");
}

void TestWhoseBlastAgainstTheImage() {
	std::printf("\nwhose blast, and whose fire timer, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe\n");
		return;
	}
	Check(Bytes(img, EXPLOSION_SECTOR_LIST, {0x53, 0x56, 0x57, 0x55, 0x81, 0xEC}) &&
	          Dword(img, EXPLOSION_SECTOR_LIST + 6) == 0xB0,
	      "the explosion's damage pass keeps a 0xC0 frame over its arguments");
	Check(Bytes(img, EXPLOSION_VEHICLE_CAUSE, {0x6A, WEAPONTYPE_EXPLOSION}) &&
	          Bytes(img, EXPLOSION_VEHICLE_CULPRIT, {0xFF, 0xB4, 0x24}) &&
	          Dword(img, EXPLOSION_VEHICLE_CULPRIT + 3) == 0xC0 + 4 + 4 * 4 + 8 &&
	          CallsTo(img, EXPLOSION_VEHICLE_DAMAGE, CVehicle__InflictDamage),
	      "and hands a car cause 18 with its fifth argument, the creator, as the culprit");
	Check(Bytes(img, PROJECTILE_BLAST_CULPRIT, {0x8B, 0x46, 0x04, 0x50, 0x6A, 0x00}) &&
	          CallsTo(img, PROJECTILE_BLAST_CALL, CExplosion__AddExplosion),
	      "a projectile's explosion is created by its source, the thrower");
	Check(Bytes(img, FIRE_TIMER_CULPRIT, {0x8B, 0x85}) &&
	          Dword(img, FIRE_TIMER_CULPRIT + 2) == offs::AUTO_SET_ON_FIRE_ENTITY &&
	          Bytes(img, FIRE_TIMER_BLOW_UP, {0xFF, 0x53, uint8_t(4 * VTABLE_BLOW_UP_CAR)}),
	      "the fire timer blows the car up through slot 29 blaming who set it alight");
	Check(Bytes(img, 0x00534768, {0xD8, 0x85}) &&
	          Dword(img, 0x0053476A) == offs::AUTO_FIRE_BLOWUP_TIMER &&
	          Bytes(img, 0x0053477A, {0xD8, 0x1D}) && Dword(img, 0x0053477C) == 0x00600730 &&
	          Float(img, 0x00600730) == VEH_FIRE_BLOWUP_MS,
	      "after five seconds on the timer the host is the one machine not holding");
}

} // namespace

int RunWreckTests() {
	TestTheShellsPlan();
	TestTheShellAgainstTheImage();
	TestWhoseBlastAgainstTheImage();
	return g_wreckFailures;
}
