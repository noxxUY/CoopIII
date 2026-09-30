// Riding in somebody else's car: the odd jobs' key (game/sidejob.h), the
// camera that followed the car a frame late and the unique jump's shot
// (game/ridecam.h, stuntshotview.h).
//
// The redirects can't run here. What can is every rule they apply and, with a
// retail exe handed over, every instruction they sit in, read back out of it.

#include "game/addresses.h"
#include "game/ridecam.h"
#include "game/sidejob.h"
#include "stuntshotview.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_rideFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_rideFailures;
}

void TestTheJobKey() {
	std::printf("\nthe sub-mission key, from each seat\n");
	Check(SubMissionKeyCounts(0, 19, false), "at the wheel, or on foot, the key is heard");
	Check(!SubMissionKeyCounts(0, 19, true), "from a passenger seat it is not");
	Check(!SubMissionKeyCounts(0, 14, true), "nor Square, which controller setup 3 uses for it");
	Check(SubMissionKeyCounts(0, 16, true) && SubMissionKeyCounts(0, 12, true),
	      "Cross and Start, the intro's skip, are a passenger's as ever");
	Check(SubMissionKeyCounts(1, 19, true), "and another pad is not the player's");
}

void TestAVehiclesStart() {
	std::printf("\na start that is its vehicle's\n");
	Check(VehicleStartGate(true, false, false) == VehicleStart::Ask,
	      "the driver's goes to the session's gate");
	Check(VehicleStartGate(true, false, true) == VehicleStart::Ask,
	      "even with a mission running, which that gate turns down itself");
	Check(VehicleStartGate(true, true, false) == VehicleStart::Hold,
	      "a passenger's waits, unclaimed");
	Check(VehicleStartGate(true, true, true) == VehicleStart::GiveUp,
	      "and goes round its loop once the session has a mission");
	Check(VehicleStartGate(false, false, false) == VehicleStart::GiveUp &&
	          VehicleStartGate(false, true, false) == VehicleStart::GiveUp,
	      "from on foot it is never claimed, a passenger who has just got out included");
}

void TestTheJumpThread() {
	std::printf("\nwhich script's camera is a unique jump's\n");
	const char usj[8]   = {'u', 's', 'j', 0, 0x16, 0, 0, 0};
	const char upper[8] = {'U', 'S', 'J', 0, 0, 0, 0, 0};
	const char hj[8]    = {'h', 'j', 0, 0, 0, 0, 0, 0};
	const char longer[8] = {'u', 's', 'j', '2', 0, 0, 0, 0};
	const char runner[8] = {'c', 'o', 'o', 'p', 'i', 'i', 'i', 0};
	Check(IsStuntShotThread(usj), "USJ, as NAME_THREAD leaves it, with whatever follows the nul");
	Check(IsStuntShotThread(upper), "and in capitals, as the script spells it");
	Check(!IsStuntShotThread(hj) && !IsStuntShotThread(longer) && !IsStuntShotThread(runner) &&
	          !IsStuntShotThread(nullptr),
	      "not the insane stunt thread, a longer name, or the runner that replays a mission");
}

void TestTheShotView() {
	std::printf("\nthe shot on a rider's camera\n");
	StuntShot s;
	Check(StuntShotOnPacket(s, true, 80, true) == StuntShotStep::Show, "a start in our car shows");
	Check(StuntShotOnPacket(s, true, 80, false) == StuntShotStep::None, "not in it, nothing");
	Check(StuntShotOnPacket(s, true, INVALID_NETID, true) == StuntShotStep::None,
	      "a car with no name, nothing");
	Check(StuntShotOnPacket(s, false, 80, true) == StuntShotStep::None,
	      "an end with nothing up, nothing");
	s.netId   = 80;
	s.sinceMs = 1000;
	Check(StuntShotOnPacket(s, false, 81, true) == StuntShotStep::None, "another car's end, nothing");
	Check(StuntShotOnPacket(s, false, 80, false) == StuntShotStep::End,
	      "its own end ends it, wherever we sit by then");
	Check(!StuntShotOver(s, true, 1000 + STUNT_SHOT_MAX_MS - 1), "held while we ride");
	Check(StuntShotOver(s, false, 1001), "down once we are out");
	Check(StuntShotOver(s, true, 1000 + STUNT_SHOT_MAX_MS), "and after the longest a jump takes");
	Check(!StuntShotOver(StuntShot{}, false, 1000000), "nothing up, nothing to take down");
	Check(StuntShotOver(s, true, 999), "a clock that went back ends it rather than holding it");
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

// Every `E8 rel32` in the code section that lands on `target`. The code
// section is the image's first 0x1E3000 bytes past the headers, which is
// where .text sits in the file and in memory alike.
std::vector<uint32_t> CallersOf(const std::vector<uint8_t> &img, uint32_t target) {
	std::vector<uint32_t> out;
	const uint32_t first = 0x00401000, last = 0x005E3000;
	for (uint32_t va = first; va + 5 <= last; ++va)
		if (img[va - IMAGE_BASE] == 0xE8 && va + 5 + Dword(img, va + 1) == target)
			out.push_back(va);
	return out;
}

void TestAgainstTheImage() {
	std::printf("\nriding in somebody else's car, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "passenger's calls against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The odd jobs' key.
	Check(CallsAt(img, IS_BUTTON_PRESSED_PadStateCall, CRunningScript__GetPadState) &&
	          Bytes(img, IS_BUTTON_PRESSED_PadStateCall - 4, {0x89, 0xD9, 0x56, 0x50}) &&
	          Bytes(img, IS_BUTTON_PRESSED_PadStateCall + 5, {0x66, 0x85, 0xC0}),
	      "IS_BUTTON_PRESSED asks GetPadState(pad, button) for the script, and tests ax");
	Check(IS_BUTTON_PRESSED_PadStateCall > IS_BUTTON_PRESSED_HANDLER &&
	          IS_BUTTON_PRESSED_PadStateCall < IS_BUTTON_PRESSED_CompareCall,
	      "inside the handler, ahead of the answer the cutscene skip takes");
	const std::vector<uint32_t> pads = CallersOf(img, CRunningScript__GetPadState);
	Check(pads.size() == 2 && pads[0] == IS_BUTTON_PRESSED_PadStateCall &&
	          pads[1] == GET_PAD_STATE_PadStateCall,
	      "and it has one other caller, GET_PAD_STATE, which is left alone");
	Check(Bytes(img, CRunningScript__GetPadState + 0x18, {0x83, 0xFA, 0x13}) &&
	          Bytes(img, CRunningScript__GetPadState + 0x21, {0xFF, 0x24, 0x95}) &&
	          Dword(img, CRunningScript__GetPadState + 0x24) == GetPadState_ButtonTable &&
	          Bytes(img, CRunningScript__GetPadState + 0x2E, {0xC2, 0x08, 0x00}),
	      "GetPadState switches on buttons 0..19 through its table, `ret 8`");
	const uint32_t rs = Dword(img, GetPadState_ButtonTable + PAD_BUTTON_RIGHTSHOCK * 4);
	const uint32_t sq = Dword(img, GetPadState_ButtonTable + PAD_BUTTON_SQUARE * 4);
	Check(Bytes(img, rs, {0x66, 0x8B, 0x40, 0x26}) && Bytes(img, sq, {0x66, 0x8B, 0x40, 0x1C}),
	      "button 19 reads RightShock (+26h) and 14 reads Square (+1Ch)");

	// The frame's camera.
	Check(CallsAt(img, CGame__Process_CameraCall, CCamera__Process) &&
	          Bytes(img, CGame__Process_CameraCall - 5, {0xB9}) &&
	          Dword(img, CGame__Process_CameraCall - 4) == TheCamera,
	      "CGame::Process calls TheCamera.Process at 0x0048C9B5");
	Check(CallsAt(img, 0x0048C97A, CWorld__Process) &&
	          CallsAt(img, 0x0048C9A7, CReplay__ShouldStandardCameraBeProcessed) &&
	          Bytes(img, 0x0048C9AC, {0x84, 0xC0, 0x74, 0x0A}),
	      "after CWorld::Process, behind the replay's say-so");
	Check(CGame__Process_CameraCall > CGame__Process && CGame__Process_CameraCall < 0x0048CA08,
	      "inside CGame::Process, which ends at 0x0048CA08");
	Check(Bytes(img, CCamera__Process, {0x53, 0x56, 0x57, 0x89, 0xCB}) &&
	          Bytes(img, 0x0046D438, {0xC7, 0x05}) && Dword(img, 0x0046D43A) == 0x00628C88 &&
	          Dword(img, 0x0046D43E) == 0x3FCCCCCD &&
	          Bytes(img, 0x0046D462, {0xC6, 0x05}) && Dword(img, 0x0046D464) == TheCamera + 0x5D,
	      "CCamera::Process: `this` in ebx, PlayerMinDist 1.6f, m_bJust_Switched = 0");
	const std::vector<uint32_t> cams = CallersOf(img, CCamera__Process);
	Check(cams.size() == 4, "four calls reach it, one of them the frame's");

	// The seats on a corrected car.
	Check(CallsAt(img, 0x004B1EE2, CPed__SetPedPositionInCar) &&
	          CallsAt(img, 0x004B1EEA, CMatrix__UpdateRW) &&
	          CallsAt(img, 0x004B1EF1, CEntity__UpdateRwFrame) &&
	          Bytes(img, 0x004B1EE7, {0x8D, 0x4E, 0x04}),
	      "CWorld::Process seats a driving ped, then its matrix to its RwFrame");
	Check(Bytes(img, 0x004B1E59, {0x80, 0xB9}) && Dword(img, 0x004B1E5B) == offs::PED_IN_VEHICLE &&
	          Bytes(img, 0x004B1E7F, {0x8B, 0xB9}) && Dword(img, 0x004B1E81) == offs::PED_MY_VEHICLE &&
	          Bytes(img, 0x004B1EAC, {0x83, 0xE8, 0x32, 0x83, 0xF8, 0x06}),
	      "for a ped in a car, by state, PED_DRIVING taking the default arm");
	Check(Bytes(img, CPed__SetPedPositionInCar, {0x53, 0x55, 0x30, 0xDB}) &&
	          Bytes(img, CPed__SetPedPositionInCar + 0x0A, {0x80, 0x3D}) &&
	          Dword(img, CPed__SetPedPositionInCar + 0x0C) == 0x0095CD5B &&
	          Bytes(img, CPed__SetPedPositionInCar + 0x20, {0x8A, 0x85}) &&
	          Dword(img, CPed__SetPedPositionInCar + 0x22) == 0x156,
	      "SetPedPositionInCar opens on the replay's mode and bChangedSeat");

	// The jump's shot.
	Check(Dword(img, g_ScriptOpcodeTable_300 + (OPCODE_POINT_CAMERA_AT_CAR - 304) * 4) ==
	          POINT_CAMERA_AT_CAR_HANDLER,
	      "POINT_CAMERA_AT_CAR is entry 40 of the 300 table");
	Check(Bytes(img, CRunningScript__ProcessCommands300To399 + 3, {0x89, 0xCD}) &&
	          Bytes(img, POINT_CAMERA_AT_CAR_HANDLER + 3, {0x89, 0xE9}),
	      "the 300 range keeps its script in ebp, and the handler hands ebp on as `this`");
	Check(CallsAt(img, POINT_CAMERA_AT_CAR_TakeControlCall, CCamera__TakeControl) &&
	          Bytes(img, POINT_CAMERA_AT_CAR_TakeControlCall - 10, {0x6A, 0x01, 0x53, 0x51, 0xB9}) &&
	          Dword(img, POINT_CAMERA_AT_CAR_TakeControlCall - 5) == TheCamera,
	      "and calls TheCamera.TakeControl(car, mode, switch, SCRIPT)");
	bool ebpKept = true;
	for (uint32_t va = POINT_CAMERA_AT_CAR_HANDLER; va < POINT_CAMERA_AT_CAR_TakeControlCall; ++va)
		if (img[va - IMAGE_BASE] == 0x5D || img[va - IMAGE_BASE] == 0xBD)
			ebpKept = false;
	Check(ebpKept, "with no pop ebp or mov ebp,imm between them");
	Check(Bytes(img, 0x004715A6, {0xC2, 0x10, 0x00}) && Bytes(img, 0x0047157F, {0xA3}) &&
	          Dword(img, 0x00471580) == TheCamera + offs::CAMERA_TARGET &&
	          Bytes(img, 0x00471529, {0x89, 0xB5}) &&
	          Dword(img, 0x0047152B) == offs::CAMERA_WHO_CONTROLS,
	      "TakeControl writes the controller and the target, `ret 10h`");
	Check(Dword(img, g_ScriptOpcodeTable_300 + (OPCODE_SET_FIXED_CAMERA_POSITION - 304) * 4) ==
	              SET_FIXED_CAMERA_POSITION_HANDLER &&
	          CallsAt(img, 0x0043F808, CCamera__SetCamPositionForFixedMode),
	      "SET_FIXED_CAMERA_POSITION calls SetCamPositionForFixedMode");
	Check(Bytes(img, CCamera__SetCamPositionForFixedMode + 0x0A, {0xD9, 0x99}) &&
	          Dword(img, CCamera__SetCamPositionForFixedMode + 0x0C) == offs::CAMERA_FIXED_SOURCE &&
	          Bytes(img, CCamera__SetCamPositionForFixedMode + 0x24, {0xD9, 0x99}) &&
	          Dword(img, CCamera__SetCamPositionForFixedMode + 0x26) == offs::CAMERA_FIXED_UP &&
	          Bytes(img, CCamera__SetCamPositionForFixedMode + 0x3C, {0xC2, 0x08, 0x00}),
	      "which stores the position at +6E0h and the offset at +6ECh, `ret 8`");
	Check(Dword(img, g_ScriptOpcodeTable_700 + (OPCODE_RESTORE_CAMERA_JUMPCUT - 700) * 4) ==
	              RESTORE_CAMERA_JUMPCUT_HANDLER &&
	          CallsAt(img, RESTORE_CAMERA_JUMPCUT_Call, CCamera__RestoreWithJumpCut) &&
	          Bytes(img, RESTORE_CAMERA_JUMPCUT_HANDLER, {0xB9}) &&
	          Dword(img, RESTORE_CAMERA_JUMPCUT_HANDLER + 1) == TheCamera,
	      "RESTORE_CAMERA_JUMPCUT is TheCamera.RestoreWithJumpCut and nothing else");
	Check(Bytes(img, CCamera__RestoreWithJumpCut + 0x18, {0xC7, 0x83}) &&
	          Dword(img, CCamera__RestoreWithJumpCut + 0x1A) == offs::CAMERA_WHO_CONTROLS &&
	          Dword(img, CCamera__RestoreWithJumpCut + 0x1E) == uint32_t(CAMCONTROL_NOBODY),
	      "which hands the camera back to nobody");
	int into = 0;
	for (uint32_t i = 0; i < 96; ++i) {
		const uint32_t to = Dword(img, g_ScriptOpcodeTable_300 + i * 4);
		if (to > POINT_CAMERA_AT_CAR_HANDLER && to <= POINT_CAMERA_AT_CAR_TakeControlCall)
			++into;
	}
	Check(into == 0, "no other entry of the 300 table lands between the handler and its call");
}

} // namespace

int RunPassengerTests() {
	g_rideFailures = 0;
	TestTheJobKey();
	TestAVehiclesStart();
	TestTheJumpThread();
	TestTheShotView();
	TestAgainstTheImage();
	return g_rideFailures;
}
