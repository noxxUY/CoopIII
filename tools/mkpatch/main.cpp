// Turns two gta3.exe files into the patch the Setup ships.
//
//   mkpatch <the-exe-you-have> <the-v1.0-exe> <out.patch>
//
// Run this yourself, on a machine that has both. The repo carries neither and
// never will - what it can carry is the differences between them, which is
// what this writes.
//
// The patch records the MD5 of both ends, so the Setup will only apply it to
// the exact build it was made from and will only keep a result whose MD5 is
// the one it was made to produce. That means a patch built from a Steam copy
// works for every other Steam copy of the same build, and refuses anything
// else with a plain sentence rather than a corrupted game.
#include "installer/core.h"

#include "launcher/core.h"

#include <cstdio>
#include <string>

using namespace coopiii;
using namespace coopiii::installer;

namespace {

unsigned long long SizeOf(const std::string &path) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return 0;
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fclose(fh);
	return size > 0 ? static_cast<unsigned long long>(size) : 0;
}

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc != 4) {
		std::printf("mkpatch <from.exe> <to-v1.0.exe> <out.patch>\n");
		std::printf("\n");
		std::printf("Builds the downgrade patch CoopIII-Setup applies. Neither exe\n");
		std::printf("goes into the patch: only the differences between them.\n");
		return 2;
	}

	const std::string from = argv[1];
	const std::string to   = argv[2];
	const std::string out  = argv[3];

	if (!launcher::FileExists(from)) {
		std::printf("no such file: %s\n", from.c_str());
		return 1;
	}
	if (!launcher::FileExists(to)) {
		std::printf("no such file: %s\n", to.c_str());
		return 1;
	}

	const std::string toMd5 = launcher::Md5File(to);
	std::printf("from: %s\n      %llu bytes, MD5 %s\n", from.c_str(), SizeOf(from),
	            launcher::Md5File(from).c_str());
	std::printf("to:   %s\n      %llu bytes, MD5 %s\n", to.c_str(), SizeOf(to), toMd5.c_str());

	// The target has to actually be v1.0 retail, or the Setup would be
	// shipping a patch to somewhere the launcher's checks will refuse anyway.
	if (toMd5 != launcher::GAME_MD5) {
		std::printf("\n");
		std::printf("That second file is not v1.0 retail.\n");
		std::printf("  expected MD5 %s\n", launcher::GAME_MD5);
		std::printf("  got          %s\n", toMd5.c_str());
		std::printf("CoopIII's addresses belong to that build only, so a patch to\n");
		std::printf("anything else would produce a game it cannot hook.\n");
		return 1;
	}

	std::string error;
	if (!MakePatch(from, to, out, &error)) {
		std::printf("\n%s\n", error.c_str());
		return 1;
	}

	PatchInfo info;
	if (!ReadPatchInfo(out, &info, &error)) {
		std::printf("\nthe patch was written but cannot be read back: %s\n", error.c_str());
		return 1;
	}

	const unsigned long long patchSize = SizeOf(out);
	std::printf("\nwrote %s\n", out.c_str());
	std::printf("  %llu bytes, %u instructions\n", patchSize, info.ops);
	std::printf("  %.1f%% of the size of the exe it produces\n",
	            info.newSize ? 100.0 * patchSize / info.newSize : 0.0);
	std::printf("\n");
	std::printf("Put it next to CoopIII-Setup.exe as gta3-v10.patch.\n");
	std::printf("\n");
	std::printf("Worth a look before you ship it: the patch holds whatever bytes\n");
	std::printf("differ between the two builds. If it is most of the exe, the two\n");
	std::printf("are too far apart for this to be a patch rather than a copy, and\n");
	std::printf("shipping it would be shipping the game.\n");
	return 0;
}
