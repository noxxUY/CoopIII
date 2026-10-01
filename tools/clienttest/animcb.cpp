// A ped's car animations when CoopIII changes his seat by hand:
// client/src/game/animcb.h.
//
// What can run here is which callbacks are dropped, which one is finished and
// when the guard lets the engine's run; with a retail exe, the callbacks and
// the two pushes are read back out of it, along with the reason fading an
// animation does not stop its callback.

#include "game/animcb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_animCbFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_animCbFailures;
}

constexpr uintptr_t ROLLING_GUARD = 0x10001000;   // stand-ins for the two wrappers
constexpr uintptr_t TRAIN_GUARD   = 0x10002000;

void TestWhichAreDropped() {
	std::printf("a ped's car animations, when his seat is changed by hand\n");
	int n = 0;
	for (uintptr_t fn : CAR_CHAIN_CALLBACKS)
		n += IsCarChainCallback(fn) ? 1 : 0;
	Check(n == 14, "the fourteen callbacks of the car chain are all known");
	Check(!IsCarChainCallback(0x004CE810) && !IsCarChainCallback(0x004D3950) &&
	          !IsCarChainCallback(0x004E68A0) && !IsCarChainCallback(0),
	      "getting up, dying and an attack are none of them");
	Check(CarCallbackReadsCarUntested(CPed__PedAnimDoorCloseRollingCB) &&
	          CarCallbackReadsCarUntested(CPed__PedSetOutTrainCB) &&
	          !CarCallbackReadsCarUntested(CPed__PedAnimGetInCB) &&
	          !CarCallbackReadsCarUntested(CPed__PedSetOutCarCB),
	      "two of them read the car with no test");

	Check(EngineCarCallback(ROLLING_GUARD, ROLLING_GUARD, TRAIN_GUARD) ==
	              CPed__PedAnimDoorCloseRollingCB &&
	          EngineCarCallback(TRAIN_GUARD, ROLLING_GUARD, TRAIN_GUARD) == CPed__PedSetOutTrainCB,
	      "a guard stands for the engine's callback it wraps");
	Check(EngineCarCallback(CPed__PedAnimAlignCB, ROLLING_GUARD, TRAIN_GUARD) ==
	              CPed__PedAnimAlignCB &&
	          EngineCarCallback(0x004CE810, ROLLING_GUARD, TRAIN_GUARD) == 0 &&
	          EngineCarCallback(0, 0, 0) == 0,
	      "the rest stand for themselves, and anything else for nothing");

	Check(PendingCarCallback(ANIM_CB_FINISH, CPed__PedAnimGetInCB, true) &&
	          PendingCarCallback(ANIM_CB_DELETE, CPed__PedSetDraggedOutCarPositionCB, true),
	      "a finish or a delete callback of the chain, on this ped, is pending");
	Check(!PendingCarCallback(ANIM_CB_NONE, CPed__PedAnimGetInCB, true) &&
	          !PendingCarCallback(ANIM_CB_FINISH, 0, true) &&
	          !PendingCarCallback(ANIM_CB_FINISH, CPed__PedAnimGetInCB, false),
	      "one already fired, one of something else or one for another ped is not");

	Check(EndPendingCarCallback(CPed__PedAnimDoorCloseRollingCB, ANIM_CB_FINISH, true) ==
	          CarCallbackEnd::FinishNow,
	      "a rolling door close is finished now, so the car gets its door back");
	Check(EndPendingCarCallback(CPed__PedAnimDoorCloseRollingCB, ANIM_CB_FINISH, false) ==
	              CarCallbackEnd::Drop &&
	          EndPendingCarCallback(CPed__PedAnimGetInCB, ANIM_CB_FINISH, true) ==
	              CarCallbackEnd::Drop &&
	          EndPendingCarCallback(CPed__PedSetDraggedOutCarPositionCB, ANIM_CB_DELETE, true) ==
	              CarCallbackEnd::Drop,
	      "but not on a car that is not a CAutomobile, and nothing else is");

	Check(!GuardedCarCallbackRuns(CPed__PedAnimDoorCloseRollingCB, false, false) &&
	          !GuardedCarCallbackRuns(CPed__PedAnimDoorCloseRollingCB, true, false) &&
	          GuardedCarCallbackRuns(CPed__PedAnimDoorCloseRollingCB, true, true),
	      "the guard runs the rolling close only on a CAutomobile");
	Check(!GuardedCarCallbackRuns(CPed__PedSetOutTrainCB, false, false) &&
	          GuardedCarCallbackRuns(CPed__PedSetOutTrainCB, true, false),
	      "and the train get-out on any car");

	const uint8_t push[] = {0x68, 0x90, 0x4B, 0x4E, 0x00};
	Check(PushesImm(push, CPed__PedAnimDoorCloseRollingCB) &&
	          !PushesImm(push, CPed__PedSetOutTrainCB),
	      "a push is read as its immediate");
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
	return ReadDword(&img[va - IMAGE_BASE]);
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
	std::printf("\nthe car chain's callbacks against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the car "
		            "chain's callbacks against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Bytes(img, CAnimBlendAssociation__SetFinishCallback,
	            {0xC7, 0x41, uint8_t(ANIM_CALLBACK_TYPE), uint8_t(ANIM_CB_FINISH), 0, 0, 0,
	             0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, uint8_t(ANIM_CALLBACK),
	             0x8B, 0x44, 0x24, 0x08, 0x89, 0x41, uint8_t(ANIM_CALLBACK_ARG)}) &&
	          Bytes(img, CAnimBlendAssociation__SetDeleteCallback,
	                {0xC7, 0x41, uint8_t(ANIM_CALLBACK_TYPE), uint8_t(ANIM_CB_DELETE)}),
	      "SetFinishCallback and SetDeleteCallback write the kind, the function and the ped");
	Check(Bytes(img, 0x0040330D, {0x8B, 0x45, uint8_t(ANIM_FLAGS), 0x83, 0xE0,
	                              uint8_t(ASSOC_DELETEFADEDOUT), 0x74, 0x2B, 0x8B, 0x45,
	                              uint8_t(ANIM_CALLBACK_TYPE), 0x83, 0xF8, 0x02, 0x74, 0x05,
	                              0x83, 0xF8, 0x01, 0x75, 0x0A, 0x8B, 0x45,
	                              uint8_t(ANIM_CALLBACK_ARG), 0x50, 0x55, 0xFF, 0x55,
	                              uint8_t(ANIM_CALLBACK)}),
	      "a faded-out association calls its finish or delete callback as it is deleted");

	const uintptr_t table[REPLAY_CB_COUNT] = {
	    0,          0x004CE810, 0x004CE8D0, 0x004D36E0, 0x004D3950, 0x004D6520,
	    0x004D7490, 0x004D7A80, CPed__PedAnimGetInCB, CPed__PedAnimDoorOpenCB,
	    CPed__PedAnimPullPedOutCB, CPed__PedAnimDoorCloseCB, CPed__PedSetInCarCB,
	    CPed__PedSetOutCarCB, CPed__PedAnimAlignCB, CPed__PedSetDraggedOutCarCB,
	    CPed__PedAnimStepOutCarCB, CPed__PedSetInTrainCB, CPed__PedSetOutTrainCB,
	    0x004E68A0, 0x004E9830, 0x0042F570, 0x0042F470, CPed__PedAnimDoorCloseRollingCB,
	    0x004D7A50, 0x004CE8A0, 0x004C6580, 0x004D6550,
	    CPed__PedSetQuickDraggedOutCarPositionCB, CPed__PedSetDraggedOutCarPositionCB};
	bool same = true;
	int  chain = 0;
	for (int i = 0; i < REPLAY_CB_COUNT; ++i) {
		same &= Dword(img, uint32_t(CReplay__CBArray + i * 4)) == table[i];
		chain += IsCarChainCallback(table[i]) ? 1 : 0;
	}
	Check(same && chain == 14,
	      "the replay's table of animation callbacks holds the fourteen where addresses.h "
	      "has them");
	Check(Bytes(img, 0x00584E76, {0x3B, 0x0C, 0x85}) && Dword(img, 0x00584E79) == CReplay__CBArray &&
	          Bytes(img, 0x00584E81, {0x83, 0xF8, uint8_t(REPLAY_CB_COUNT)}),
	      "and FindCBFunctionID walks thirty of them");
	Check(Dword(img, uint32_t(CReplay__CBArray + REPLAY_CB_ROLLING_DOOR * 4)) ==
	              CPed__PedAnimDoorCloseRollingCB &&
	          Dword(img, uint32_t(CReplay__CBArray + REPLAY_CB_OUT_TRAIN * 4)) ==
	              CPed__PedSetOutTrainCB,
	      "the two guarded ones' slots");

	Check(Bytes(img, 0x004E4B9B, {0x8B, 0x98, 0x10, 0x03, 0x00, 0x00}) &&
	          Bytes(img, 0x004E4BAB, {0x8A, 0x83, 0xF6, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x004E4BD3, {0x80, 0xA3, 0xCB, 0x01, 0x00, 0x00, 0xFE}),
	      "the rolling door close reads the car's bLowVehicle with no test (the crash at "
	      "0x004E4BAB) and gives back the front left door");
	Check(Bytes(img, 0x004E3741, {0x8B, 0x83, 0x10, 0x03, 0x00, 0x00, 0xD9, 0xEE, 0xD9, 0x40,
	                              0x14}),
	      "the train get-out reads the train's matrix with no test");
	Check(Bytes(img, 0x004DE150, {0x85, 0xED, 0x74, 0x0E}) &&
	          Bytes(img, 0x004DE520, {0x85, 0xF6, 0x74, 0x0E}) &&
	          Bytes(img, 0x004DECA0, {0x85, 0xDB, 0x74, 0x1E}) &&
	          Bytes(img, 0x004DF1C1, {0x85, 0xED, 0x74, 0x1E}) &&
	          Bytes(img, 0x004CF230, {0x85, 0xED, 0x75, 0x0C}) &&
	          Bytes(img, 0x004E3295, {0x83, 0xBB, 0x10, 0x03, 0x00, 0x00, 0x00, 0x75, 0x02}) &&
	          Bytes(img, 0x004DF5E0, {0x85, 0xDB, 0x75, 0x1C}),
	      "align, door open, get in, door close, set in car, set in train and step out test "
	      "for no car first");

	for (const CarCallbackGuardSite &s : CAR_CALLBACK_GUARD_SITES) {
		Check(PushesImm(&img[s.push - IMAGE_BASE], s.original) &&
		          CallsAt(img, uint32_t(s.push + 5), CAnimBlendAssociation__SetFinishCallback),
		      "each guarded callback is pushed straight before SetFinishCallback");
		int pushes = 0;
		for (uint32_t va = 0x00401000; va + 5 <= 0x005E3000; ++va)
			if (PushesImm(&img[va - IMAGE_BASE], s.original))
				++pushes;
		Check(pushes == 1, "and that push is the only one of it in .text");
	}
	Check(Bytes(img, 0x004F0024, {0x89, 0xC1, 0x56}) && Bytes(img, 0x004E36AD, {0x55}),
	      "with the ped as its argument");
}

} // namespace

int RunAnimCallbackTests() {
	g_animCbFailures = 0;
	TestWhichAreDropped();
	TestAgainstTheImage();
	return g_animCbFailures;
}
