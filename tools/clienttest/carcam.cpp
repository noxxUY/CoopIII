// A car camera mod beside CoopIII (client/src/game/carcam.h).
//
// The chained call can't run here. What can is who a call site is said to
// belong to, when the gun of the followed car is kept, and, with a retail exe
// handed over, every instruction the chain and the mods it sits beside rely
// on, read back out of it.

#include "game/addresses.h"
#include "game/carcam.h"
#include "game/emergencyaddr.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_carCamFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_carCamFailures;
}

void TestWhoOwnsTheCall() {
	std::printf("\nwho the car camera call belongs to\n");
	const uintptr_t retail = CCam__Process_Cam_On_A_String;
	Check(WhoOwnsCarCamCall(0xE8, retail, retail, CodeHome::Exe) == CarCamOwner::Retail,
	      "the call the image shipped with is the game's");
	Check(WhoOwnsCarCamCall(0xE8, 0x10003B20, retail, CodeHome::OtherModule) ==
	          CarCamOwner::Mod,
	      "a call into another module is a mod's, and is chained");
	Check(WhoOwnsCarCamCall(0xE8, 0x0045C100, retail, CodeHome::Exe) == CarCamOwner::Unknown,
	      "a call somewhere else in gta3.exe is nobody we can vouch for");
	Check(WhoOwnsCarCamCall(0xE8, 0x7FFE0000, retail, CodeHome::Nowhere) ==
	          CarCamOwner::Unknown,
	      "nor one into memory no module owns");
	Check(WhoOwnsCarCamCall(0xE9, retail, retail, CodeHome::Exe) == CarCamOwner::Unknown &&
	          WhoOwnsCarCamCall(0x90, 0, retail, CodeHome::Nowhere) == CarCamOwner::Unknown,
	      "and what is no longer a call is left alone");
}

void TestWhoseGun() {
	std::printf("\nthe gun of the car the camera follows\n");
	Check(CarCameraKeepsOffGun(true, MODEL_RHINO, false) &&
	          CarCameraKeepsOffGun(true, FIRETRUCK_MODEL, false),
	      "a tank or a fire truck somebody else drives keeps its gun");
	Check(!CarCameraKeepsOffGun(true, MODEL_RHINO, true) &&
	          !CarCameraKeepsOffGun(true, FIRETRUCK_MODEL, true),
	      "the one we drive is the camera's to turn, as in single player");
	Check(!CarCameraKeepsOffGun(true, 90, false), "a car with no gun has nothing to keep");
	Check(!CarCameraKeepsOffGun(false, MODEL_RHINO, false),
	      "and nothing but an automobile has the gun's fields");
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

// A `ret 10h` within `len` bytes of `from`, before any other function's
// entry could be: the callee is callee-cleaned with four dwords.
bool Ret10Within(const std::vector<uint8_t> &img, uint32_t from, uint32_t len) {
	for (uint32_t va = from; va < from + len; ++va)
		if (Bytes(img, va, {0xC2, 0x10, 0x00}))
			return true;
	return false;
}

// One arm of CCam::Process's switch: `lea eax,[esp+18h] / mov ecx,ebx`, the
// three dword pushes, `push eax`, then the call and the jump out.
bool ArmCalls(const std::vector<uint8_t> &img, uint32_t arm, uint32_t call, uint32_t callee) {
	return Bytes(img, arm, {0x8D, 0x44, 0x24, 0x18, 0x89, 0xD9, 0xFF, 0x74, 0x24, 0x04, 0xFF,
	                        0xB3, 0xD0, 0x00, 0x00, 0x00, 0xFF, 0x74, 0x24, 0x10, 0x50}) &&
	       call == arm + 0x15 && CallsAt(img, call, callee) &&
	       Bytes(img, call + 5, {0xE9}) && call + 10 + Dword(img, call + 6) == 0x00459D2A;
}

// The callee: `push ebx / push esi / mov ebx,ecx / push edi / sub esp,..`,
// then CamTargetEntity into a register and its type tested for 2.
bool OpensOnTheTarget(const std::vector<uint8_t> &img, uint32_t fn, uint32_t typeCmp) {
	return Bytes(img, fn, {0x53, 0x56, 0x89, 0xCB, 0x57, 0x83, 0xEC}) &&
	       Bytes(img, fn + 8, {0x8B}) && Dword(img, fn + 10) == CAM_TARGET_ENTITY &&
	       (Bytes(img, typeCmp, {0x80, 0xFA, 0x02}) || Bytes(img, typeCmp, {0x3C, 0x02}));
}

void TestAgainstTheImage() {
	std::printf("\nthe car camera's call, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "car camera's call against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, CCam__Process_ModeSwitch,
	            {0x0F, 0xBF, 0x43, 0x0C, 0x48, 0x83, 0xF8, 0x2B, 0x0F, 0x87}) &&
	          Bytes(img, CCam__Process_ModeSwitch + 0x0E, {0xFF, 0x24, 0x85}) &&
	          Dword(img, CCam__Process_ModeSwitch + 0x11) == CCam__Process_ModeTable,
	      "CCam::Process switches on Mode - 1 through its table, 44 modes");
	Check(Dword(img, CCam__Process_ModeTable + (CAM_MODE_CAM_ON_A_STRING - 1) * 4) ==
	              CCam__Process_CamOnAStringArm &&
	          Dword(img, CCam__Process_ModeTable + (CAM_MODE_BEHINDBOAT - 1) * 4) ==
	              CCam__Process_BehindBoatArm,
	      "MODE_CAM_ON_A_STRING (18) and MODE_BEHINDBOAT (22) have their arms where we say");
	Check(Dword(img, CCam__Process_ModeTable + (CAM_MODE_FOLLOWPED - 1) * 4) == 0x00459A5E,
	      "and FOLLOWPED's, the passenger aim's, is another arm");
	Check(ArmCalls(img, CCam__Process_CamOnAStringArm, CCam__Process_CamOnAStringCall,
	               CCam__Process_Cam_On_A_String),
	      "the car camera's arm is `this` in ecx, four dwords and one call, at 0x00459A54");
	Check(ArmCalls(img, CCam__Process_BehindBoatArm, CCam__Process_BehindBoatCall,
	               CCam__Process_BehindBoat),
	      "the boat camera's is the same, at 0x00459B36");
	Check(OpensOnTheTarget(img, CCam__Process_Cam_On_A_String, 0x0045C0A8) &&
	          Ret10Within(img, CCam__Process_Cam_On_A_String, 0x360),
	      "Process_Cam_On_A_String reads CamTargetEntity, wants a vehicle, `ret 10h`");
	Check(OpensOnTheTarget(img, CCam__Process_BehindBoat, 0x0045B487) &&
	          Ret10Within(img, CCam__Process_BehindBoat, 0x9E0),
	      "Process_BehindBoat the same");
	const std::vector<uint32_t> onString = CallersOf(img, CCam__Process_Cam_On_A_String);
	const std::vector<uint32_t> boat     = CallersOf(img, CCam__Process_BehindBoat);
	Check(onString.size() == 1 && onString[0] == CCam__Process_CamOnAStringCall && boat.size() == 1 &&
	          boat[0] == CCam__Process_BehindBoatCall,
	      "each has that one caller and no other, so the call is the whole car camera");
	Check(Bytes(img, CCam__WellBufferMe, {0x83, 0xEC, 0x08, 0x8B, 0x54, 0x24, 0x10}) &&
	          CallersOf(img, CCam__WellBufferMe).size() == 18,
	      "WellBufferMe, the mods' third patch, opens `sub esp,8` and has eighteen callers");

	// Nothing of ours on any site the mods write.
	const uint32_t ours[] = {CGame__Process_CameraCall, POINT_CAMERA_AT_CAR_TakeControlCall,
	                         RESTORE_CAMERA_JUMPCUT_Call, FollowPedMouse_LineOfSightCall1,
	                         FollowPedMouse_LineOfSightCall2, FollowPedMouse_SphereCall1,
	                         FollowPedMouse_SphereCall2};
	const uint32_t theirs[] = {CCam__WellBufferMe, CCam__Process_CamOnAStringCall,
	                           CCam__Process_BehindBoatCall, 0x0052260E, 0x0053D628,
	                           0x005225D2, 0x0048BFB0};
	bool apart = true;
	for (uint32_t a : ours)
		for (uint32_t b : theirs)
			if (a < b + 5 && b < a + 5)
				apart = false;
	Check(apart, "none of our camera call sites overlaps any five bytes SACarCam writes");
	Check(CallsAt(img, 0x0052260E, 0x00493070) && CallsAt(img, 0x0053D628, 0x004930C0) &&
	          CallsAt(img, 0x005225D2, 0x004930C0) && CallsAt(img, 0x0048BFB0, 0x004735A0),
	      "and those are the gun controls' stick calls and one in CGame::Initialise");

	// The gun and its motor.
	Check(Bytes(img, 0x0053D6CE, {0xB9}) && Dword(img, 0x0053D6CF) == 0x0095CDBE &&
	          Bytes(img, 0x0053D6D6, {0x8B, 0x43, uint8_t(offs::PHYSICAL_AUDIO_ENTITY), 0x6A,
	                                  uint8_t(SOUND_CAR_TANK_TURRET_ROTATE), 0x50}) &&
	          CallsAt(img, TANK_TURRET_SOUND_CALL, CAudioEngine__PlayOneShot),
	      "TankControl's turret sound is DMAudio.PlayOneShot(m_audioEntityId at +64h, 1Ah)");
	Check(CallsAt(img, 0x0057C85A, cAudioManager__PlayOneShot) &&
	          Bytes(img, 0x0057C84F, {0xB9}) && Dword(img, 0x0057C850) == 0x00880FC0,
	      "which hands it straight to cAudioManager::PlayOneShot");
	Check(Bytes(img, 0x0057A512, {0x83, 0x7C, 0x24, 0x34, 0x00, 0x0F, 0x8C}) &&
	          Bytes(img, 0x0057A51D, {0x81, 0x7C, 0x24, 0x34, 0xC8, 0x00, 0x00, 0x00, 0x0F,
	                                  0x8D}),
	      "and that plays nothing for an entity below zero, or past the 200th");
}

} // namespace

int RunCarCameraTests() {
	g_carCamFailures = 0;
	TestWhoOwnsTheCall();
	TestWhoseGun();
	TestAgainstTheImage();
	return g_carCamFailures;
}
