// The Setup's working parts, with no window and no network.
//
// The two that matter most are the ones that touch a player's files: the
// downgrade patch, which rewrites their gta3.exe, and SHA-256, which is the
// only thing standing between a download and their game folder. A third,
// InstallJob, is what actually copies those files into a game folder - it ran
// with zero coverage until this suite grew TestInstallJob below.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;
using namespace coopiii::installer;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what) {
	std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
	if (!ok)
		++g_failures;
}

// ---- SHA-256 --------------------------------------------------------------

void TestSha256() {
	std::printf("SHA-256\n");

	auto of = [](const char *text) { return Sha256(text, std::strlen(text)); };

	Check(of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	      "the empty string");
	Check(of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      "\"abc\"");
	Check(of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
	          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
	      "the 56 character vector, which spans two blocks");

	// A megabyte of 'a', which is the vector that catches a broken length
	// field.
	const std::string million(1000000, 'a');
	Check(Sha256(million.data(), million.size()) ==
	          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
	      "a million a's");
}

// ---- the patch ------------------------------------------------------------

std::vector<uint8_t> Noise(size_t length, uint32_t seed) {
	std::mt19937                            rng(seed);
	std::uniform_int_distribution<unsigned> byte(0, 255);
	std::vector<uint8_t>                    out(length);
	for (uint8_t &b : out)
		b = static_cast<uint8_t>(byte(rng));
	return out;
}

void TestPatch() {
	std::printf("the downgrade patch\n");

	// Two "builds": the same file with a few runs changed, which is the shape
	// two builds of one program have.
	std::vector<uint8_t> oldBytes = Noise(300000, 1);
	std::vector<uint8_t> newBytes = oldBytes;
	for (size_t at : {1000u, 50000u, 123456u, 299000u})
		for (size_t i = 0; i < 64 && at + i < newBytes.size(); ++i)
			newBytes[at + i] ^= 0xFF;
	// ...and a stretch that only exists in the new one.
	newBytes.insert(newBytes.begin() + 200000, 4096, 0x5A);

	std::vector<uint8_t> patch, rebuilt;
	std::string          error;

	Check(MakePatchBuffers(oldBytes, newBytes, &patch, &error), "a patch is built");
	Check(ApplyPatchBuffers(oldBytes, patch, &rebuilt, &error), "and applies");
	Check(rebuilt == newBytes, "what comes out is what went in");

	char line[160];
	std::snprintf(line, sizeof(line), "the patch is %.1f%% of the file it produces (%zu bytes)",
	              100.0 * patch.size() / newBytes.size(), patch.size());
	// The whole point of a patch is that it is not the file. Two builds that
	// share this much should come out well under a tenth.
	Check(patch.size() < newBytes.size() / 10, line);

	// It must refuse anything but the build it was made from. This is what
	// keeps a downgrade from corrupting somebody's game.
	std::vector<uint8_t> wrong = oldBytes;
	wrong[12345] ^= 0x01;
	std::vector<uint8_t> nothing;
	Check(!ApplyPatchBuffers(wrong, patch, &nothing, &error),
	      "a file with one byte changed is refused");
	Check(error.find("MD5") != std::string::npos, "and the reason says why");

	std::vector<uint8_t> shorter(oldBytes.begin(), oldBytes.end() - 1);
	Check(!ApplyPatchBuffers(shorter, patch, &nothing, &error),
	      "a file of the wrong size is refused");

	Check(!ApplyPatchBuffers(oldBytes, Noise(200, 9), &nothing, &error),
	      "something that is not a patch at all is refused");

	// A truncated patch must not walk off the end of its own buffer.
	std::vector<uint8_t> truncated(patch.begin(), patch.begin() + patch.size() / 2);
	Check(!ApplyPatchBuffers(oldBytes, truncated, &nothing, &error),
	      "a half-written patch is refused");

	// Identical files: a patch of pure copies.
	std::vector<uint8_t> same;
	Check(MakePatchBuffers(oldBytes, oldBytes, &patch, &error) &&
	          ApplyPatchBuffers(oldBytes, patch, &same, &error) && same == oldBytes,
	      "a patch between two identical files works too");
}

// ---- the manifest ---------------------------------------------------------

void TestManifest() {
	std::printf("the manifest\n");

	const Manifest &m = BuiltInManifest();
	char            line[128];
	std::snprintf(line, sizeof(line), "the built-in manifest parses (%zu components)",
	              m.components.size());
	Check(m.components.size() == 12, line);

	const Component *coop = m.Find("coopiii");
	Check(coop != nullptr, "CoopIII is in it");
	if (coop) {
		Check(coop->required, "and it is required");
		Check(coop->source == "local", "and comes from beside the Setup, not a download");
		Check(coop->Ready(), "so it is ready to install");
		Check(coop->files.size() == 3, "with three files");
	}

	const Component *loader = m.Find("asiloader");
	Check(loader != nullptr && loader->required, "Ultimate ASI Loader is required");
	if (loader) {
		Check(loader->Downloadable(), "and is a download");
		// The URLs and hashes are left for the project owner to fill in once
		// each licence has been read. Until then the Setup shows the component
		// and refuses to install it, which is the honest behaviour.
		Check(!loader->Ready(), "but has no download set yet, so it is not ready");
	}

	int downloads = 0, ready = 0;
	for (const Component &c : m.components) {
		if (c.Downloadable())
			++downloads;
		if (c.Ready())
			++ready;
	}
	Check(downloads == 11, "eleven of the twelve are downloads");
	Check(ready == 1, "and only CoopIII itself can be installed today");

	// The reader has to be strict: a manifest that half-parsed would install
	// half an Essential Pack and say it was done.
	Manifest    bad;
	std::string error;
	Check(!bad.Parse("", &error), "an empty manifest is refused");
	Check(!bad.Parse("{\"components\": []}", &error), "one with no components is refused");
	Check(!bad.Parse("{\"components\": [{\"name\": \"no id\"}]}", &error),
	      "a component with no id is refused");
	Check(!bad.Parse("{\"components\": [{\"id\": \"x\", \"name\": \"x\"", &error),
	      "a truncated file is refused");

	Manifest good;
	Check(good.Parse("{\"note\": \"ignored\", \"components\": [{\"id\": \"a\", \"name\": \"A\","
	                 " \"required\": true, \"somethingNew\": {\"deep\": [1, 2]}}]}",
	                 &error),
	      "a manifest with keys this build does not know still parses");
	Check(good.components.size() == 1 && good.components[0].required,
	      "and the keys it does know landed");
}

// ---- InstallJob -------------------------------------------------------

// A fresh directory under the OS temp folder, so the test never touches a
// real game install and cleans up after itself either way.
std::string TempDir(const char *leaf) {
	char base[MAX_PATH] = {0};
	GetTempPathA(MAX_PATH, base);
	std::string dir = launcher::Join(base, leaf);
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir;
}

void RemoveTree(const std::string &dir) {
	// SHFileOperation wants a double-NUL-terminated list.
	std::string from = dir;
	from.push_back('\0');
	SHFILEOPSTRUCTA op = {};
	op.wFunc           = FO_DELETE;
	op.pFrom           = from.c_str();
	op.fFlags          = FOF_NO_UI;
	SHFileOperationA(&op);
}

// How many files a directory holds, not counting "." and "..". Used to prove
// a refused download left nothing behind, rather than checking one guessed
// filename and hoping that was the only place it could have landed.
int CountFiles(const std::string &dir) {
	WIN32_FIND_DATAA data;
	HANDLE           find = FindFirstFileA((dir + "\\*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	int count = 0;
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			++count;
	} while (FindNextFileA(find, &data));
	FindClose(find);
	return count;
}

std::vector<uint8_t> ReadWhole(const std::string &path) {
	std::vector<uint8_t> out;
	FILE                *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return out;
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	out.resize(size > 0 ? static_cast<size_t>(size) : 0);
	if (!out.empty())
		std::fread(out.data(), 1, out.size(), fh);
	std::fclose(fh);
	return out;
}

// Waits for a job to stop running, the way the window's frame loop would
// see it happen over many frames - polling Snapshot(), never touching the
// job's internals.
Progress RunToCompletion(InstallJob &job, uint32_t timeoutMs = 5000) {
	const auto start = std::chrono::steady_clock::now();
	Progress   p      = job.Snapshot();
	while (p.running) {
		if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs))
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		p = job.Snapshot();
	}
	return p;
}

void TestInstallJob() {
	std::printf("InstallJob\n");

	// A synthetic payload standing in for the three files the real Setup
	// embeds - this is the one path a fake CoopIII.asi can exercise honestly,
	// since InstallJob does not care what is inside a file, only that it
	// arrives whole. Whether the Setup's own payload.inc actually holds
	// CoopIII.asi is xmake's job (the "Check the Setup carries what it
	// should" step in release.yml), not this suite's.
	const std::vector<uint8_t> asi{'C', '3', 'A', 'S', 'I', 0x90, 0x90, 0x01, 0x02, 0x03};
	const std::vector<uint8_t> ini{'[', 'c', 'o', 'o', 'p', 'i', 'i', 'i', ']', '\n'};
	const std::vector<uint8_t> exe(4096, 0xCC);   // stands in for the launcher

	SetPayloadReader([&](const std::string &name, std::vector<uint8_t> *out) {
		if (name == "CoopIII.asi") { *out = asi; return true; }
		if (name == "CoopIII.ini") { *out = ini; return true; }
		if (name == "coopiii-launcher.exe") { *out = exe; return true; }
		return false;
	});

	// ---- the one component that is actually ready today -------------------
	{
		const std::string dir = TempDir("coopiii-installtest-local");

		InstallJob job;
		job.Start(dir, {"coopiii"});
		Check(job.Running() || job.Snapshot().total == 1, "the job accepted the component");

		const Progress p = RunToCompletion(job);
		Check(!p.running, "and finished within the timeout");
		Check(!p.failed && p.finished == 1 && p.total == 1,
		      "CoopIII installed clean - the only component with a url today");
		Check(p.steps.size() == 1 && p.steps[0].state == StepState::Done,
		      "its step is marked Done");

		Check(ReadWhole(launcher::Join(dir, "CoopIII.asi")) == asi,
		      "CoopIII.asi landed in the game folder byte for byte");
		Check(ReadWhole(launcher::Join(dir, "CoopIII.ini")) == ini,
		      "so did CoopIII.ini");
		Check(ReadWhole(launcher::Join(dir, "coopiii-launcher.exe")) == exe,
		      "so did the launcher");

		bool sawOkDetail = false;
		for (const std::string &line : p.details)
			if (line.find("[ ok ]") != std::string::npos && line.find("CoopIII") != std::string::npos)
				sawOkDetail = true;
		Check(sawOkDetail, "and the details log says so");

		RemoveTree(dir);
	}

	// ---- a component nobody has pointed at a download yet -----------------
	//
	// Eleven of the twelve components in the built-in manifest are exactly
	// this today (TestManifest, above). The Setup has to refuse them
	// cleanly rather than crash, hang, or silently skip them and call the
	// install a success.
	{
		const std::string dir = TempDir("coopiii-installtest-noturl");

		InstallJob job;
		job.Start(dir, {"asiloader"});
		const Progress p = RunToCompletion(job);

		Check(!p.running, "a component with no url still finishes rather than hanging");
		Check(p.failed && p.finished == 0 && p.total == 1,
		      "and is reported as failed, not silently skipped");
		Check(p.steps.size() == 1 && p.steps[0].state == StepState::Failed,
		      "its step is marked Failed");
		Check(p.steps[0].note.find("no download") != std::string::npos,
		      "with a note that says why, not just \"Failed\"");
		Check(CountFiles(dir) == 0, "and nothing was written into the game folder for it");

		RemoveTree(dir);
	}

	// ---- an unknown id --------------------------------------------------
	//
	// Start() looks every id up in the manifest before touching the disk;
	// one that is not there (a stale CoopIII.ini from an older Setup version,
	// say) is dropped rather than crashing the lookup.
	{
		const std::string dir = TempDir("coopiii-installtest-unknown");
		InstallJob        job;
		job.Start(dir, {"not-a-real-component"});
		const Progress p = RunToCompletion(job);
		Check(!p.running && p.total == 0,
		      "an id the manifest does not have is dropped, not run");
		RemoveTree(dir);
	}

	// ---- cancelling ---------------------------------------------------
	//
	// Whatever point cancellation catches a step at, the job has to leave
	// every step in a final state - nothing stuck at Waiting or Working once
	// Snapshot().running goes false, which is what the window's Cancel
	// button promises.
	{
		const std::string dir = TempDir("coopiii-installtest-cancel");
		InstallJob        job;
		job.Start(dir, {"coopiii", "asiloader", "cleo", "modloader"});
		job.Cancel();
		const Progress p = RunToCompletion(job);

		Check(!p.running, "a cancelled job still finishes");
		bool allFinal = true;
		for (const StepProgress &s : p.steps)
			if (s.state == StepState::Waiting || s.state == StepState::Working)
				allFinal = false;
		Check(allFinal, "and leaves nothing Waiting or Working behind");
		RemoveTree(dir);
	}

	SetPayloadReader(nullptr);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	TestSha256();
	TestPatch();
	TestManifest();
	TestInstallJob();

	std::printf("\n%s\n",
	            g_failures == 0 ? "all installer checks passed" : "installer checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
