// The ids a pedestrian's talk and wait-state overlay may carry on the wire
// (protocol.h, IsPedOverlayAnim), against the animations CPed::SetWaitState
// loads in the retail exe, and the packet's layout.

#include "game/addresses.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_overlayFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_overlayFailures;
}

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

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> b) {
	size_t i = 0;
	for (uint8_t x : b)
		if (img[va - IMAGE_BASE + i++] != x)
			return false;
	return true;
}

void TestTheIdsOnTheWire() {
	std::printf("\nwhich animations a pedestrian's overlay may name\n");
	Check(IsPedOverlayAnim(ANIM_STD_CHAT) && IsPedOverlayAnim(ANIM_STD_HAILTAXI) &&
	          IsPedOverlayAnim(ANIM_STD_DUCK_DOWN) && IsPedOverlayAnim(ANIM_STD_HANDSUP) &&
	          IsPedOverlayAnim(ANIM_STD_HANDSCOWER),
	      "the five wait-state animations are allowed");
	Check(!IsPedOverlayAnim(ANIM_NONE) && !IsPedOverlayAnim(ANIM_STD_WALK) &&
	          !IsPedOverlayAnim(ANIM_STD_IDLE) && !IsPedOverlayAnim(ANIM_STD_KO_FRONT) &&
	          !IsPedOverlayAnim(ANIM_STD_NUM) && !IsPedOverlayAnim(0x0D) && !IsPedOverlayAnim(0xA9),
	      "nothing else is, a knockdown least of all");
	Check(sizeof(C_PedOverlay) == 9 && sizeof(S_PedOverlay) == 9 && OP_C_PED_OVERLAY == 0x0A &&
	          OP_S_PED_OVERLAY == 0x0B,
	      "the pair is 9 bytes, on 0x0A and 0x0B");
}

void TestAgainstTheImage() {
	std::printf("\nthe wait-state animations against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "wait-state animations against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	const uint16_t ids[5] = {ANIM_STD_HANDSCOWER, ANIM_STD_HANDSUP, ANIM_STD_DUCK_DOWN,
	                         ANIM_STD_HAILTAXI, ANIM_STD_CHAT};
	for (int i = 0; i < 5; ++i)
		Check(Bytes(img, uint32_t(WAIT_STATE_ANIM_LOADS[i]),
		            {0xBF, uint8_t(ids[i]), 0x00, 0x00, 0x00}) &&
		          Bytes(img, uint32_t(WAIT_STATE_ANIM_LOADS[i]) - 8,
		                {0x81, 0xFF, uint8_t(ANIM_STD_NUM), 0x00, 0x00, 0x00}),
		      "SetWaitState swaps ANIM_STD_NUM for this id before it blends");
}

} // namespace

int RunPedOverlayTests() {
	g_overlayFailures = 0;
	TestTheIdsOnTheWire();
	TestAgainstTheImage();
	return g_overlayFailures;
}
