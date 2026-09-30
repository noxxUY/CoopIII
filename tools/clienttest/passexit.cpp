// A passenger gets out by his own door (client/src/game/passexit.h).
//
// The two taken calls can't run here. What can is which door each seat
// leaves by, what the room test is told, and, with a retail exe handed over,
// the two sites and everything around them the change relies on, read back
// out of it: that SetExitCar maps the seats to those doors, that a no from
// the room test is what sends the front passenger to the driver's door, and
// that nothing jumps into the six bytes the room test is taken on.

#include "game/addresses.h"
#include "game/passexit.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_passExitFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_passExitFailures;
}

void TestTheDoors() {
	std::printf("\nthe door each seat gets out by\n");
	Check(OwnExitDoor(0, false) == offs::CAR_DOOR_RF,
	      "the front passenger leaves by the front right door");
	Check(OwnExitDoor(1, false) == offs::CAR_DOOR_LR && OwnExitDoor(2, false) == offs::CAR_DOOR_RR,
	      "the rear seats by the rear doors on their own sides, left then right");
	Check(OwnExitDoor(EXIT_SEAT_NONE, false) == offs::CAR_DOOR_RF,
	      "a ped in the car but in no seat leaves where he is drawn, the front passenger's "
	      "door, not the driver's the engine would give him");
	Check(OwnExitDoor(EXIT_SEAT_DRIVER, false) == 0,
	      "the driver's door is left to the engine: nothing changes at the wheel");
	Check(OwnExitDoor(3, false) == 0 && OwnExitDoor(7, false) == 0,
	      "a seat past the third slot has no door of its own and is the engine's to place");
	Check(OwnExitDoor(0, true) == 0 && OwnExitDoor(2, true) == 0 &&
	          OwnExitDoor(EXIT_SEAT_NONE, true) == 0,
	      "and so is every seat of a bus, which all leave at the front");

	Check(ExitDoorOnRight(offs::CAR_DOOR_RF) && ExitDoorOnRight(offs::CAR_DOOR_RR) &&
	          !ExitDoorOnRight(offs::CAR_DOOR_LR) && !ExitDoorOnRight(offs::CAR_DOOR_LF),
	      "the seat key steps a rider down on his door's side: right for RF and RR, left for LR");
}

void TestTheRoomAnswer() {
	std::printf("\nwhat SetExitCar is told about the room at a door\n");
	Check(ExitRoomAnswer(false, offs::CAR_DOOR_RF, offs::CAR_DOOR_RF),
	      "no room at the held door is answered yes, so he does not go round to the driver's");
	Check(ExitRoomAnswer(true, offs::CAR_DOOR_RF, offs::CAR_DOOR_RF),
	      "room there is room");
	Check(!ExitRoomAnswer(false, offs::CAR_DOOR_LF, offs::CAR_DOOR_RF),
	      "any other door keeps the engine's no");
	Check(!ExitRoomAnswer(false, offs::CAR_DOOR_RF, 0) && ExitRoomAnswer(true, offs::CAR_DOOR_RF, 0),
	      "and with no door held, every answer is the engine's: the driver, the NPC logic, a bus");
}

void TestANewDriver() {
	std::printf("\nsomebody takes the wheel of a car players ride in\n");
	Check(NewDriverSendsOut(false, false),
	      "a pedestrian riding along is sent out, as in single player");
	Check(!NewDriverSendsOut(true, false), "our player keeps his seat");
	Check(!NewDriverSendsOut(false, true),
	      "and so does another player's copy, whose seat his own machine decides");
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
		if (CallsAt(img, va, target))
			out.push_back(va);
	return out;
}

// `mov ebx,<door>` / `jmp` at one arm of SetExitCar's seat test.
bool DoorArm(const std::vector<uint8_t> &img, uint32_t at, uint8_t door) {
	return Bytes(img, at, {0xBB, door, 0x00, 0x00, 0x00});
}

void TestAgainstTheImage() {
	std::printf("\nthe exit key's SetExitCar and its room test, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "passenger's door against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The call site.
	const std::vector<uint32_t> callers = CallersOf(img, CPed__SetExitCar);
	Check(callers.size() == 1 && callers[0] == LEAVE_CAR_SET_EXIT_CAR_CALL,
	      "SetExitCar has one caller, the LEAVE_CAR arm at 0x004DA157, which the exit key "
	      "and every script exit reach");
	Check(Bytes(img, LEAVE_CAR_SET_EXIT_CAR_CALL - 7, {0x89, 0xD9, 0x6A, 0x00, 0x52, 0xDD, 0xD8}),
	      "`mov ecx,ebx / push 0 / push edx / fstp st(0)` before it: the ped in ecx, the "
	      "car and door 0 pushed");
	Check(Bytes(img, CPed__SetExitCar + 0x7D, {0xC2, 0x08, 0x00}),
	      "and SetExitCar returns `ret 8`, as the replacement does");

	// The seat -> door mapping OwnExitDoor copies.
	Check(Bytes(img, 0x004E10F7, {0x39, 0xAE, 0xA4, 0x01, 0x00, 0x00}) &&
	          DoorArm(img, 0x004E10FF, offs::CAR_DOOR_LF),
	      "pDriver (+1A4h) leaves by door 0Fh, LF");
	Check(Bytes(img, 0x004E1106, {0x39, 0xAE, 0xA8, 0x01, 0x00, 0x00}) &&
	          DoorArm(img, 0x004E110E, offs::CAR_DOOR_RF),
	      "pPassengers[0] (+1A8h) by 0Bh, RF");
	Check(Bytes(img, 0x004E1115, {0x39, 0xAE, 0xAC, 0x01, 0x00, 0x00}) &&
	          DoorArm(img, 0x004E111D, offs::CAR_DOOR_LR),
	      "pPassengers[1] (+1ACh) by 10h, LR");
	Check(Bytes(img, 0x004E1124, {0x39, 0xAE, 0xB0, 0x01, 0x00, 0x00}) &&
	          DoorArm(img, 0x004E112C, offs::CAR_DOOR_RR),
	      "pPassengers[2] (+1B0h) by 0Ch, RR");
	Check(Bytes(img, 0x004E10EC, {0xBB, 0x0F, 0x00, 0x00, 0x00}),
	      "and anybody in none of those starts from LF, the driver's door");
	Check(Bytes(img, 0x004E10E6, {0x8A, 0x86, 0xF6, 0x01, 0x00, 0x00, 0xBB, 0x0F, 0x00, 0x00,
	                              0x00, 0xD0, 0xE8, 0x24, 0x01, 0x75}),
	      "a bus (+1F6h bit 1) keeps LF for every seat");
	Check(Bytes(img, 0x004E10A8, {0x85, 0xDB}) && Bytes(img, 0x004E10E0, {0x0F, 0x85}),
	      "a door passed in skips the seat test: `test ebx,ebx / jne`");

	// The room test.
	Check(Bytes(img, SET_EXIT_CAR_ROOM_CALL - 5, {0x8B, 0x39, 0x6A, 0x00, 0x53}) &&
	          Bytes(img, SET_EXIT_CAR_ROOM_CALL, {0xFF, 0x97, 0x80, 0x00, 0x00, 0x00}),
	      "`mov edi,[ecx] / push 0 / push ebx / call [edi+80h]`: the room test on the car in "
	      "ecx, for the door just chosen");
	Check(Bytes(img, SET_EXIT_CAR_ROOM_CALL + 6, {0x89, 0xF1, 0x88, 0x44, 0x24, 0x08}),
	      "its answer is read out of al and nothing after it needs edi");
	Check(Bytes(img, SET_EXIT_CAR_SIDE_ROOM_CALL - 5, {0x8B, 0x39, 0x6A, 0x00, 0x53}) &&
	          Bytes(img, SET_EXIT_CAR_SIDE_ROOM_CALL, {0xFF, 0x97, 0x80, 0x00, 0x00, 0x00}),
	      "the second one, for the far side, only runs after a no from the first");
	Check(Dword(img, CAutomobile__vtable + VEH_VT_IS_ROOM_FOR_PED_TO_LEAVE_CAR) ==
	          CAutomobile__IsRoomForPedToLeaveCar,
	      "CAutomobile's slot 0x80 is IsRoomForPedToLeaveCar, 0x0053C5B0");
	Check(Bytes(img, CAutomobile__IsRoomForPedToLeaveCar, {0x53, 0x56, 0x89, 0xCE, 0x55}),
	      "a __thiscall taking the car in ecx");

	// A no at the front passenger's door, and where it goes.
	Check(Dword(img, 0x005F943C) == 0x004E12EB && Dword(img, 0x005F943C + 4 * 4) == 0x004E129A,
	      "a no jumps through 5F943Ch: RF to 004E12EB, LF to 004E129A");
	Check(Bytes(img, 0x004E12EB, {0x8B, 0xBE, 0xA4, 0x01, 0x00, 0x00, 0x85, 0xFF, 0x75, 0x13}) &&
	          DoorArm(img, 0x004E1301, offs::CAR_DOOR_LF),
	      "at RF with nobody at the wheel: door := LF, the driver's");
	Check(Dword(img, 0x005F9424) == 0x004E137A && DoorArm(img, 0x004E137A, offs::CAR_DOOR_LF),
	      "and with somebody there a player is swapped to LF all the same (5F9424h)");
	Check(Bytes(img, 0x004E1275, {0x80, 0x7C, 0x24, 0x08, 0x00, 0x0F, 0x85}) &&
	          0x004E1280 + Dword(img, 0x004E127C) == 0x004E13E0,
	      "while a yes goes straight to the exit at 004E13E0, by the door it asked about");

	// Nothing lands inside the six bytes.
	int into = 0;
	for (uint32_t va = 0x00401000; va + 6 <= 0x005E3000; ++va) {
		const uint8_t b = img[va - IMAGE_BASE];
		uint32_t      to;
		if (b == 0xE8 || b == 0xE9)
			to = va + 5 + Dword(img, va + 1);
		else if (b == 0xEB || (b >= 0x70 && b <= 0x7F))
			to = va + 2 + static_cast<int8_t>(img[va + 1 - IMAGE_BASE]);
		else if (b == 0x0F && img[va + 1 - IMAGE_BASE] >= 0x80 && img[va + 1 - IMAGE_BASE] <= 0x8F)
			to = va + 6 + Dword(img, va + 2);
		else
			continue;
		if (to > SET_EXIT_CAR_ROOM_CALL && to < SET_EXIT_CAR_ROOM_CALL + SET_EXIT_CAR_ROOM_CALL_LEN)
			++into;
	}
	Check(into == 0, "nothing in the image jumps into the middle of the room test's six bytes");

	// A new driver's passengers.
	Check(Bytes(img, 0x004CF4C7, {0x8B, 0x83, 0x64, 0x01, 0x00, 0x00, 0x83, 0xF8, 0x0F, 0x75}),
	      "PedSetInCarCB clears the car only for a ped whose objective is ENTER_CAR_AS_DRIVER");
	Check(Bytes(img, 0x004CF4D6, {0x8B, 0x8C, 0xB5, 0xA8, 0x01, 0x00, 0x00, 0x85, 0xC9, 0x74}),
	      "walking pPassengers[esi] (+1A8h), the passenger in ecx");
	Check(Bytes(img, 0x004CF4E1, {0x80, 0xB9, 0x60, 0x01, 0x00, 0x00, 0x01, 0x75, 0x08}),
	      "only the RANDOM_CHAR ones (+160h == 1), which the player ped is");
	Check(Bytes(img, NEW_DRIVER_PASSENGER_LEAVE_CALL - 3, {0x55, 0x6A, 0x0D}) &&
	          CallsAt(img, NEW_DRIVER_PASSENGER_LEAVE_CALL, CPed__SetObjective),
	      "`push ebp / push 0Dh / call SetObjective`: LEAVE_CAR, the car, at 0x004CF4ED");
	Check(Bytes(img, NEW_DRIVER_PASSENGER_LEAVE_CALL + 5, {0x46, 0x0F, 0xB6, 0x85, 0xCC, 0x01}),
	      "and the loop goes on in esi and ebp, which the replacement keeps");
	Check(Bytes(img, CPed__SetObjective, {0x53, 0x89, 0xCB}) && CallersOf(img, CPed__SetObjective).size() > 1,
	      "SetObjective takes the ped in ecx (`push ebx / mov ebx,ecx`) and has callers of its own besides, so only this call is taken");
}

void TestAPassengerCanAlwaysGetOut() {
	std::printf("\na passenger the exit key did not get out\n");
	Check(PassengerLetOut(true, false, true, 0.0f, false, PASSENGER_EXIT_WAIT_MS),
	      "still in his seat 400 ms after he pressed it, in a car standing still: let out");
	Check(!PassengerLetOut(true, false, true, 0.0f, false, PASSENGER_EXIT_WAIT_MS - 1),
	      "not before: the engine's own exit gets its frames first");
	Check(!PassengerLetOut(true, true, true, 0.0f, false, 5000),
	      "nor once the engine is getting him out by his door");
	Check(!PassengerLetOut(false, false, true, 0.0f, false, 5000), "nor once he is out");
	Check(!PassengerLetOut(true, false, true, PASSENGER_EXIT_MAX_SPEED, false, 5000) &&
	          PassengerLetOut(true, false, true, PASSENGER_EXIT_MAX_SPEED - 0.01f, false, 5000),
	      "only as slow as the engine lets anybody out, 0.17 of move speed");
	Check(!PassengerLetOut(true, false, false, 0.0f, false, 5000),
	      "never from a boat or a bus, which the engine lets out its own way");
	Check(!PassengerLetOut(true, false, true, 0.0f, true, 5000), "nor in the middle of a cutscene");

	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check CPlayerInfo's "
		            "exit key against one\n");
		return;
	}
	const uint32_t lock = static_cast<uint32_t>(offs::VEH_DOOR_LOCK);
	const uint8_t  l0 = lock & 0xFF, l1 = (lock >> 8) & 0xFF, l2 = (lock >> 16) & 0xFF, l3 = lock >> 24;
	Check(Bytes(img, 0x004A0212, {0x83, 0xF8, 0x05, 0x0F, 0x84}) &&
	          Bytes(img, 0x004A021B, {0x83, 0xFA, 0x06, 0x0F, 0x84}) &&
	          Bytes(img, 0x004A0224, {0x83, 0xB9, l0, l1, l2, l3, 0x04, 0x0F, 0x84}),
	      "CPlayerInfo's exit key gives up on a wrecked car, a moving train and "
	      "CARLOCK_LOCKED_PLAYER_INSIDE (`cmp dword [ecx+224h],4` at 0x004A0224)");
	Check(Bytes(img, 0x004A025E, {0xD8, 0x1D, 0x68, 0x6A, 0x5F, 0x00}) &&
	          Bytes(img, 0x005F6A68, {0x7B, 0x14, 0x2E, 0x3E}),
	      "and on a car moving faster than 0.17 (the float at 0x005F6A68)");
}

} // namespace

int RunPassengerExitTests() {
	g_passExitFailures = 0;
	TestTheDoors();
	TestTheRoomAnswer();
	TestANewDriver();
	TestAgainstTheImage();
	TestAPassengerCanAlwaysGetOut();
	return g_passExitFailures;
}
