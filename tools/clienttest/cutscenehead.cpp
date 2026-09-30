// A cutscene head with no animation (game/cutscenehead.h): what counts as an
// animated hierarchy, where a held head is said to stand - and, with a copy
// of the retail exe, why a head can be walked with none, and the three calls
// game/cutscenehead.cpp takes.

#include "game/addresses.h"
#include "game/cutscenehead.h"
#include "game/leadcheck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii::game;

namespace {

int g_headFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_headFailures;
}

void Put32(uint8_t *at, uint32_t v) { std::memcpy(at, &v, 4); }

void TestWhatCountsAsAnimated() {
	std::printf("\na cutscene head's hierarchy\n");
	Check(HEAD_HIER_CURRENT_ANIM == offs::HANIM_CURRENT_ANIM &&
	          HEAD_HIER_TO_MATRIX == offs::HANIM_KEYFRAME_TO_MATRIX && HEAD_HIER_SPAN == offs::HANIM_READ_SPAN,
	      "the two fields are where addresses.h has them");
	Check(!HeadHierarchyAnimated(nullptr), "no hierarchy: not animated, and nothing is read");

	// As RpHAnimHierarchyCreate leaves it: flags and node count set, the
	// animation and the callback zero, everything else whatever it is.
	uint8_t hier[0x5C];
	std::memset(hier, 0xCD, sizeof hier);
	Put32(hier + 0x00, 0);
	Put32(hier + 0x04, 20);
	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0);
	Put32(hier + HEAD_HIER_TO_MATRIX, 0);
	Check(!HeadHierarchyAnimated(hier),
	      "a fresh one (what CREATE_CUTSCENE_HEAD leaves until SET_HEAD_ANIM): held");

	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0x35E2F000);
	Check(!HeadHierarchyAnimated(hier), "an animation with no key-frame callback: still held");
	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0);
	Put32(hier + HEAD_HIER_TO_MATRIX, 0x005CDEE0);
	Check(!HeadHierarchyAnimated(hier), "a callback with no animation: still held");

	// What RpHAnimHierarchySetCurrentAnim writes: both, together.
	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0x35E2F000);
	Put32(hier + HEAD_HIER_TO_MATRIX, 0x005CDEE0);
	Check(HeadHierarchyAnimated(hier), "after SET_HEAD_ANIM: the engine's own calls, unchanged");

	// The crash's own registers: EDX, the animation, was 0.
	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0);
	Check(!HeadHierarchyAnimated(hier), "the crash's hierarchy (EDX 0 at 0x005B14F7) is held");
}

// A participant's frames around a replayed head, the order the replay can
// run them in: the world walks the head between the two instructions, or
// before the second ever comes.
void TestTheReplayOrder() {
	std::printf("\na replayed head, frame by frame\n");
	uint8_t hier[HEAD_HIER_SPAN] = {};
	int     walkedWithNone       = 0;
	int     animated             = 0;
	auto    worldStep            = [&] {
		if (HeadHierarchyAnimated(hier))
			++animated;
		else
			++walkedWithNone;
	};
	// Frame 1: CREATE_CUTSCENE_HEAD ran (the head is in the world at once),
	// and the packet with its SET_HEAD_ANIM is still on the socket thread.
	worldStep();
	// Frame 2: SET_HEAD_ANIM.
	Put32(hier + HEAD_HIER_CURRENT_ANIM, 0x1000);
	Put32(hier + HEAD_HIER_TO_MATRIX, 0x2000);
	worldStep();
	worldStep();
	Check(walkedWithNone == 1 && animated == 2,
	      "the frame between them holds the head; from SET_HEAD_ANIM on it runs as the engine's");
}

void TestWhereAHeldHeadStands() {
	std::printf("\nwhere a held head is said to stand\n");
	int         a = 0, b = 0, c = 0, stray = 0;
	const void *objects[CUTSCENE_OBJECTS_MAX + 2] = {&a, &b, &c};
	Check(PlaceOfHead(&b, objects, 3, CUTSCENE_OBJECTS_MAX, true) == HeadPlace::InLoadedScene,
	      "one of the loaded scene's objects");
	Check(PlaceOfHead(&stray, objects, 3, CUTSCENE_OBJECTS_MAX, true) == HeadPlace::NotInScene,
	      "a scene is loaded and the head is not in it: left over");
	Check(PlaceOfHead(&b, objects, 3, CUTSCENE_OBJECTS_MAX, false) == HeadPlace::NoScene,
	      "no scene loaded: left over, whatever the array still holds");
	Check(PlaceOfHead(&a, objects, 0, CUTSCENE_OBJECTS_MAX, true) == HeadPlace::NotInScene,
	      "an emptied list holds nothing");
	objects[CUTSCENE_OBJECTS_MAX] = &stray;
	Check(PlaceOfHead(&stray, objects, CUTSCENE_OBJECTS_MAX + 1, CUTSCENE_OBJECTS_MAX, true) ==
	          HeadPlace::NotInScene,
	      "a count past the array's fifty reads no further than fifty");
	Check(PlaceOfHead(&a, nullptr, 3, CUTSCENE_OBJECTS_MAX, true) == HeadPlace::NotInScene,
	      "no array, nothing read");
	Check(std::strlen(DescribeHeadPlace(HeadPlace::InLoadedScene)) > 0 &&
	          std::strlen(DescribeHeadPlace(HeadPlace::NotInScene)) > 0 &&
	          std::strlen(DescribeHeadPlace(HeadPlace::NoScene)) > 0,
	      "each has words for the log");
}

// ---- the bytes in the real exe ----------------------------------------------------------

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

// File offset = VA - image base in this image (.text, .rdata and .data all
// sit at their virtual offsets in the file).
const uint8_t *At(const std::vector<uint8_t> &img, uintptr_t va) { return &img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uintptr_t va) {
	uint32_t v;
	std::memcpy(&v, At(img, va), 4);
	return v;
}

bool BytesAt(const std::vector<uint8_t> &img, uintptr_t va, std::initializer_list<uint8_t> bytes) {
	size_t i = 0;
	for (uint8_t b : bytes)
		if (At(img, va)[i++] != b)
			return false;
	return true;
}

bool Calls(const std::vector<uint8_t> &img, uintptr_t site, uintptr_t target) {
	return RelCallAt(At(img, site), site, target);
}

constexpr uint32_t TEXT_BEGIN = 0x00401000;
constexpr uint32_t TEXT_END   = 0x005E3238;

int CallsTo(const std::vector<uint8_t> &img, uintptr_t target) {
	int n = 0;
	for (uint32_t va = TEXT_BEGIN; va + 5 < TEXT_END; ++va)
		if (RelCallAt(At(img, va), va, target))
			++n;
	return n;
}

// Every rel32 call, jmp and jcc in .text whose target lands strictly inside
// [site, site + 5), which a redirect of the five bytes would cut through.
int BranchesInto(const std::vector<uint8_t> &img, uintptr_t site) {
	int n = 0;
	for (uint32_t va = TEXT_BEGIN; va + 6 < TEXT_END; ++va) {
		const uint8_t *p   = At(img, va);
		int64_t        dst = -1;
		if (p[0] == 0xE8 || p[0] == 0xE9) {
			int32_t rel;
			std::memcpy(&rel, p + 1, 4);
			dst = int64_t(va) + 5 + rel;
		} else if (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F) {
			int32_t rel;
			std::memcpy(&rel, p + 2, 4);
			dst = int64_t(va) + 6 + rel;
		}
		if (dst > int64_t(site) && dst < int64_t(site) + 5)
			++n;
	}
	return n;
}

void TestTheBytesInTheImage() {
	std::printf("\nthe cutscene head in gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "bytes against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// Which object the crash was in.
	const uintptr_t vt = CCutsceneHead__vtable;
	Check(BytesAt(img, 0x004BA5F5, {0xC7, 0x00, 0x08, 0x7C, 0x5F, 0x00}) &&
	          BytesAt(img, 0x004BA8C3, {0xC7, 0x06, 0x08, 0x7C, 0x5F, 0x00}) &&
	          BytesAt(img, 0x004BA8F3, {0xC7, 0x03, 0x08, 0x7C, 0x5F, 0x00}),
	      "0x005F7C08 is stamped by CCutsceneHead's ctor and its two destructor bodies");
	Check(Dword(img, vt + 5 * 4) == CCutsceneHead__CreateRwObject &&
	          Dword(img, vt + 8 * 4) == CCutsceneHead__ProcessControl &&
	          Dword(img, vt + 13 * 4) == CCutsceneHead__Render,
	      "its table: CreateRwObject at +14h, ProcessControl at +20h, Render at +34h");
	Check(BytesAt(img, 0x004B1B97, {0x8B, 0x39, 0xFF, 0x57, 0x20}) &&
	          BytesAt(img, 0x004B1BE2, {0x8B, 0x39, 0xFF, 0x57, 0x20}),
	      "CWorld::Process's walks call +20h with the vtable in edi - the dump's EDI 0x005F7C08");

	// The fault.
	Check(BytesAt(img, RpHAnimHierarchyAddAnimTime, {0xD9, 0x44, 0x24, 0x08}) &&
	          BytesAt(img, 0x005B149B, {0x8B, 0x5C, 0x24, 0x14}) &&
	          BytesAt(img, 0x005B14AA, {0x8B, 0x53, 0x08}) &&
	          BytesAt(img, 0x005B14F7, {0x8B, 0x42, 0x0C}),
	      "AddAnimTime(hier, t): ebx = hier, edx = [hier+8], and 0x005B14F7 reads [edx+0Ch]");
	Check(BytesAt(img, 0x005B1788, {0x8B, 0xAC, 0x24, 0x1C, 0x09, 0x00, 0x00}) &&
	          BytesAt(img, 0x005B199D, {0x8B, 0x45, 0x40, 0x3D, 0xE0, 0xDE, 0x5C, 0x00}) &&
	          BytesAt(img, 0x005B1B1C, {0xFF, 0xD0}),
	      "UpdateHierarchyMatrices(hier) calls [hier+40h] when it is not the stock callback");

	// Why a head can have neither.
	Check(BytesAt(img, 0x005B110B, {0x33, 0xFF}) && BytesAt(img, 0x005B1114, {0x89, 0x7E, 0x08}) &&
	          BytesAt(img, 0x005B11CE, {0x89, 0x7E, 0x40}),
	      "RpHAnimHierarchyCreate leaves the animation and the callback 0");
	Check(BytesAt(img, 0x005B1D6D, {0x68, 0x00, 0x20, 0x5B, 0x00}) &&
	          Calls(img, 0x005B2034, RpHAnimHierarchyCreate) && BytesAt(img, 0x005B208B, {0x89, 0x47, 0x04}),
	      "and the frame plugin's copy makes a new one for every clone");
	Check(Calls(img, 0x004BA685, 0x005B1050),
	      "which CCutsceneHead::CreateRwObject hands the skin (RpSkinAtomicSetHAnimHierarchy)");
	Check(BytesAt(img, RpHAnimHierarchySetCurrentAnim + 0x0A, {0x89, 0x50, 0x08}) &&
	          BytesAt(img, 0x005B1220, {0x8B, 0x71, 0x08, 0x89, 0x70, 0x40}),
	      "SetCurrentAnim is what fills both");
	Check(Calls(img, 0x004BA73E, RpHAnimHierarchySetCurrentAnim) &&
	          BytesAt(img, 0x004BA6EB, {0x74, 0x67}) && BytesAt(img, 0x004BA733, {0x74, 0x10}),
	      "PlayAnimation calls it only when the .anm is in cuts.dir and its chunk is found");
	Check(Calls(img, 0x00404DA8, CCutsceneHead__PlayAnimation) && CallsTo(img, CCutsceneHead__PlayAnimation) == 1 &&
	          BytesAt(img, 0x00404D8C, {0x68, 0xD0, 0xD9, 0x70, 0x00}) &&
	          std::memcmp(At(img, 0x005EBEF0), "%s_%s", 6) == 0,
	      "and only SetHeadAnim calls PlayAnimation, with \"<scene>_<name>\"");
	Check(Calls(img, SET_HEAD_ANIM_SetCall, CCutsceneMgr__SetHeadAnim) &&
	          Calls(img, CREATE_CUTSCENE_HEAD_AddCall, CCutsceneMgr__AddCutsceneHead),
	      "SET_HEAD_ANIM and CREATE_CUTSCENE_HEAD are two handlers, two instructions");
	Check(Calls(img, 0x00404CE8, 0x004BA5E0) && Calls(img, 0x00404CFC, CWorld__Add),
	      "AddCutsceneHead builds the head and puts it in the world at once");
	Check(!CallIn(At(img, CCutsceneMgr__CreateCutsceneObject), CCutsceneMgr__CreateCutsceneObject,
	              0x00404CCF - CCutsceneMgr__CreateCutsceneObject, CWorld__Add) &&
	          BytesAt(img, 0x00404CCE, {0xC3}),
	      "where CreateCutsceneObject leaves a body out of it until START_CUTSCENE");

	// The scene's list, which the one log line reads.
	Check(BytesAt(img, 0x00404916, {0x8B, 0x04, 0x95, 0x70, 0x21, 0x86, 0x00}) &&
	          BytesAt(img, 0x004048F6, {0x83, 0x2D, 0xA4, 0x2F, 0x94, 0x00, 0x01}) &&
	          Dword(img, 0x00404CC3) == CCutsceneMgr__ms_pCutsceneObjects &&
	          Dword(img, 0x00404CBC) == CCutsceneMgr__ms_numCutsceneObjs,
	      "ms_pCutsceneObjects at 0x00862170, counted by 0x00942FA4");
	Check(BytesAt(img, 0x004B1AF9, {0x83, 0xFB, 0x32}), "fifty of them");
	Check(BytesAt(img, 0x004048E6, {0x80, 0x3D}) && Dword(img, 0x004048E8) == CCutsceneMgr__ms_running,
	      "DeleteCutsceneData does nothing unless 0x0095CD95 says a scene is loaded");

	// The three calls taken, and that what they hand over is what the
	// replacements take.
	Check(Calls(img, 0x004BA7CC, GetFirstAtomic) && Calls(img, 0x004BA7D3, RpSkinAtomicGetHAnimHierarchy) &&
	          BytesAt(img, 0x004BA7E5, {0x89, 0xC1}) &&
	          BytesAt(img, 0x004BA7E7, {0x50, 0xD9, 0x1C, 0x24, 0x51}) &&
	          Calls(img, HEAD_ADD_ANIM_TIME_CALL, RpHAnimHierarchyAddAnimTime) &&
	          BytesAt(img, 0x004BA7F1, {0x59, 0x59, 0x81, 0xC4, 0x90, 0x00, 0x00, 0x00, 0x5B, 0xC3}),
	      "ProcessControl: `push t / push hier / call AddAnimTime / pop / pop`, result unused");
	Check(Calls(img, 0x004BA86E, GetFirstAtomic) && Calls(img, 0x004BA875, RpSkinAtomicGetHAnimHierarchy) &&
	          BytesAt(img, 0x004BA87B, {0x50}) && Calls(img, HEAD_UPDATE_MATRICES_CALL, RpHAnimUpdateHierarchyMatrices) &&
	          BytesAt(img, 0x004BA881, {0x59, 0x89, 0xD9}),
	      "Render: `push hier / call UpdateHierarchyMatrices / pop / mov ecx,ebx`, result unused");
	Check(Calls(img, HEAD_OBJECT_RENDER_CALL, object::CObject__Render) &&
	          BytesAt(img, 0x004BA889, {0x81, 0xC4, 0x90, 0x00, 0x00, 0x00, 0x5B, 0xC3}),
	      "then CObject::Render, __thiscall with nothing on the stack");
	Check(BytesAt(img, GetFirstAtomic, {0x83, 0xEC, 0x08, 0xC7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x00, 0x00}) &&
	          BytesAt(img, RpSkinAtomicGetHAnimHierarchy,
	                  {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D, 0x90, 0x3C, 0x66, 0x00, 0x8B, 0x04, 0x01, 0xC3}),
	      "GetFirstAtomic and RpSkinAtomicGetHAnimHierarchy: one-argument cdecl, as the guard calls them");
	Check(CallsTo(img, RpHAnimHierarchyAddAnimTime) == 4 && CallsTo(img, RpHAnimUpdateHierarchyMatrices) == 1,
	      "the head's are the only calls from outside RpHAnim");
	Check(HEAD_UPDATE_MATRICES_CALL + 8 == HEAD_OBJECT_RENDER_CALL,
	      "the matrices and the draw sit side by side in Render");
	Check(BranchesInto(img, HEAD_ADD_ANIM_TIME_CALL) == 0 && BranchesInto(img, HEAD_UPDATE_MATRICES_CALL) == 0 &&
	          BranchesInto(img, HEAD_OBJECT_RENDER_CALL) == 0,
	      "nothing branches into the middle of the three five-byte calls");
}

} // namespace

int RunCutsceneHeadTests() {
	TestWhatCountsAsAnimated();
	TestTheReplayOrder();
	TestWhereAHeldHeadStands();
	TestTheBytesInTheImage();
	return g_headFailures;
}
