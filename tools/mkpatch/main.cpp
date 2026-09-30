// Turns two gta3.exe files into the downgrade patch the Setup fetches.
//
//   mkpatch [--max-stored <percent>] <the-exe-you-have> <the-v1.0-exe> <out.c3patch> [name]
//
// It refuses to write a patch that stores more than 25% of v1.0 as itself
// (kMaxStoredFraction): past that it is a copy of the game, not a patch.
//
// Run this yourself, on a machine that has both. The repo carries neither and
// never will - what it can carry is the differences between them, which is
// what this writes. docs/installer.md says where each input comes from.
//
// The patch records the MD5 of both ends, so the Setup will only apply it to
// the exact build it was made from and will only keep a result whose MD5 is
// the one it was made to produce. That means a patch built from a Steam copy
// works for every other Steam copy of the same build, and refuses anything
// else with a plain sentence rather than a corrupted game.
//
// It ends by printing the lines to paste into installer/assets/components.json.
#include "installer/core.h"

#include "launcher/core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

std::string Leaf(const std::string &path) {
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// --max-stored <percent> moves the ceiling; it does not remove it.
	double                   maxStored = kMaxStoredFraction;
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--max-stored") == 0 && i + 1 < argc) {
			maxStored = std::atof(argv[++i]) / 100.0;
			continue;
		}
		args.push_back(argv[i]);
	}

	if ((args.size() != 3 && args.size() != 4) || maxStored <= 0.0 || maxStored > 1.0) {
		std::printf("mkpatch [--max-stored <percent>] <from.exe> <to-v1.0.exe> <out.c3patch> [build name]\n");
		std::printf("\n");
		std::printf("Builds the downgrade patch CoopIII-Setup applies. Neither exe\n");
		std::printf("goes into the patch: only the differences between them.\n");
		std::printf("Refuses a patch that stores more than %.0f%% of v1.0 as itself.\n",
		            kMaxStoredFraction * 100.0);
		return 2;
	}

	const std::string from = args[0];
	const std::string to   = args[1];
	const std::string out  = args[2];
	const std::string name = args.size() == 4 ? args[3] : "Steam";

	if (!launcher::FileExists(from)) {
		std::printf("no such file: %s\n", from.c_str());
		return 1;
	}
	if (!launcher::FileExists(to)) {
		std::printf("no such file: %s\n", to.c_str());
		return 1;
	}

	const std::string fromMd5 = launcher::Md5File(from);
	const std::string toMd5   = launcher::Md5File(to);
	std::printf("from: %s\n      %llu bytes, MD5 %s\n", from.c_str(), SizeOf(from), fromMd5.c_str());
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
	if (fromMd5 == toMd5) {
		std::printf("\nThose are the same file; there is nothing to patch.\n");
		return 1;
	}

	std::string          error;
	std::vector<uint8_t> oldBytes, newBytes, patch;
	if (!ReadWholeFile(from, &oldBytes, &error) || !ReadWholeFile(to, &newBytes, &error) ||
	    !MakePatchBuffers(oldBytes, newBytes, &patch, &error)) {
		std::printf("\n%s\n", error.c_str());
		return 1;
	}

	PatchInfo info;
	if (!ReadPatchInfoBuffer(patch, &info, &error)) {
		std::printf("\nthe patch cannot be read back: %s\n", error.c_str());
		return 1;
	}

	// The guard. The bytes stored as themselves are the parts of v1.0 that
	// exist nowhere in the exe the player already has. Past this ceiling the
	// file is no longer a set of differences but a copy of Rockstar's game,
	// and it is not written at all, so it cannot end up in a release by
	// accident. The Steam exe is the case this is for: it is wrapped in
	// SteamStub, its code is encrypted, and a patch from it measured 86.5%.
	if (StoredFraction(info) > maxStored) {
		std::printf("\nNOT WRITTEN: this patch would store %u of the %u bytes of v1.0 as\n",
		            info.extraBytes, info.newSize);
		std::printf("themselves (%.1f%%, over the %.0f%% ceiling). A patch that big is a copy of\n",
		            StoredFraction(info) * 100.0, maxStored * 100.0);
		std::printf("the game, and CoopIII does not ship Rockstar's executable in any form.\n");
		std::printf("It usually means the source exe is wrapped or encrypted (the Steam exe is\n");
		std::printf("wrapped in SteamStub), and removing that is not something CoopIII does.\n");
		std::printf("Send players of that build to a downgrade guide instead: the manifest's\n");
		std::printf("\"downgrader\" entry, docs/installer.md section 2.\n");
		return 1;
	}

	if (!WriteFileAtomic(out, patch, &error)) {
		std::printf("\n%s\n", error.c_str());
		return 1;
	}

	const unsigned long long patchSize = SizeOf(out);
	const std::string        sha       = Sha256File(out);
	std::printf("\nwrote %s\n", out.c_str());
	std::printf("  %llu bytes, %u control entries, verified by applying it\n", patchSize,
	            info.controls);
	std::printf("  %.1f%% of the size of the exe it produces\n",
	            info.newSize ? 100.0 * patchSize / info.newSize : 0.0);
	std::printf("  %u bytes (%.1f%%) of the output are stored as themselves\n", info.extraBytes,
	            info.newSize ? 100.0 * info.extraBytes / info.newSize : 0.0);
	std::printf("  SHA-256 %s\n", sha.c_str());
	std::printf("\n");
	std::printf("Worth a look before you ship it: the stored-as-themselves bytes are the\n");
	std::printf("parts of v1.0 that exist nowhere in the exe the player already has. If\n");
	std::printf("that is most of the exe, the two builds are too far apart for this to be\n");
	std::printf("a patch rather than a copy, and shipping it would be shipping the game.\n");
	std::printf("\n");
	std::printf("Publish it as a release asset named %s, then add to\n", Leaf(out).c_str());
	std::printf("installer/assets/components.json:\n\n");
	std::printf("  \"builds\": [ ...\n");
	std::printf("    {\"md5\": \"%s\", \"size\": %llu, \"name\": \"%s\"}\n", fromMd5.c_str(),
	            SizeOf(from), name.c_str());
	std::printf("  ],\n");
	std::printf("  \"patches\": [\n");
	std::printf("    {\"from\": \"%s\", \"name\": \"%s\", \"file\": \"%s\",\n", fromMd5.c_str(),
	            name.c_str(), Leaf(out).c_str());
	std::printf("     \"url\": \"https://github.com/noxxUY/CoopIII/releases/latest/download/%s\",\n",
	            Leaf(out).c_str());
	std::printf("     \"sha256\": \"%s\"}\n", sha.c_str());
	std::printf("  ],\n");
	return 0;
}
