// The Setup's working parts, with no window.
//
// The ones that matter most touch a player's files: the downgrade patch,
// which rewrites their gta3.exe, SHA-256, which is the only thing between a
// download and their game folder, the zip reader, and the install record that
// Uninstall trusts to put everything back.
//
//   installertest                  everything, offline, in a temp folder
//   installertest --online <dir>   also installs the real Essential Pack from
//                                  the internet into a scratch game folder
//                                  under <dir>, then uninstalls it
//   installertest --measure <old> <new>
//                                  builds a patch between two real files and
//                                  reports its size, without writing it
//
// The downgrade round trip needs a real v1.0 gta3.exe, from COOPIII_GTA3_EXE
// (the same variable clienttest reads). Without it that part is skipped.
#include "installer/core.h"

#include "launcher/core.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

std::vector<uint8_t> Noise(size_t length, uint32_t seed) {
	std::mt19937                            rng(seed);
	std::uniform_int_distribution<unsigned> byte(0, 255);
	std::vector<uint8_t>                    out(length);
	for (uint8_t &b : out)
		b = static_cast<uint8_t>(byte(rng));
	return out;
}

std::vector<uint8_t> Bytes(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> ReadWhole(const std::string &path) {
	std::vector<uint8_t> out;
	ReadWholeFile(path, &out, nullptr);
	return out;
}

bool WriteWhole(const std::string &path, const std::vector<uint8_t> &bytes) {
	return WriteFileAtomic(path, bytes, nullptr);
}

std::string TempDir(const char *leaf) {
	char base[MAX_PATH] = {0};
	GetTempPathA(MAX_PATH, base);
	std::string dir = launcher::Join(base, leaf);
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir;
}

void RemoveTree(const std::string &dir) {
	std::string from = dir;
	from.push_back('\0');
	SHFILEOPSTRUCTA op = {};
	op.wFunc           = FO_DELETE;
	op.pFrom           = from.c_str();
	op.fFlags          = FOF_NO_UI;
	SHFileOperationA(&op);
}

// Every file under `dir`, relative, sorted.
void ListTree(const std::string &dir, const std::string &prefix, std::vector<std::string> *out) {
	WIN32_FIND_DATAA data;
	HANDLE           find = FindFirstFileA((dir + "\\*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do {
		const std::string name = data.cFileName;
		if (name == "." || name == "..")
			continue;
		const std::string rel = prefix.empty() ? name : prefix + "\\" + name;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			out->push_back(rel + "\\");
			ListTree(dir + "\\" + name, rel, out);
		} else {
			out->push_back(rel);
		}
	} while (FindNextFileA(find, &data));
	FindClose(find);
	std::sort(out->begin(), out->end());
}

std::vector<std::string> Tree(const std::string &dir) {
	std::vector<std::string> out;
	ListTree(dir, "", &out);
	return out;
}

Progress RunToCompletion(InstallJob &job, uint32_t timeoutMs = 20000) {
	const auto start = std::chrono::steady_clock::now();
	Progress   p     = job.Snapshot();
	while (p.running) {
		if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs))
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		p = job.Snapshot();
	}
	return p;
}

void PrintDetails(const Progress &p) {
	for (const std::string &line : p.details)
		std::printf("      %s\n", line.c_str());
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

	const std::string million(1000000, 'a');
	Check(Sha256(million.data(), million.size()) ==
	          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
	      "a million a's");
}

// ---- the patch ------------------------------------------------------------

// Something shaped like two compiles of one program: the second has a block
// of new code in the middle, and every "address" after it has moved by the
// size of that block. A copy/insert delta has to store nearly all of that as
// new bytes; this format should not.
void MakeTwoBuilds(std::vector<uint8_t> *oldBytes, std::vector<uint8_t> *newBytes) {
	*oldBytes = Noise(400000, 7);
	// Plant a 4-byte little-endian "address" every 16 bytes.
	for (size_t at = 0; at + 4 <= oldBytes->size(); at += 16) {
		const uint32_t address = 0x00400000u + static_cast<uint32_t>(at);
		std::memcpy(oldBytes->data() + at, &address, 4);
	}
	*newBytes = *oldBytes;
	const size_t   cut   = 150000;
	const uint32_t shift = 0x1230;
	for (size_t at = cut; at + 4 <= newBytes->size(); at += 16) {
		uint32_t address;
		std::memcpy(&address, newBytes->data() + at, 4);
		address += shift;
		std::memcpy(newBytes->data() + at, &address, 4);
	}
	const std::vector<uint8_t> inserted = Noise(shift, 8);
	newBytes->insert(newBytes->begin() + cut, inserted.begin(), inserted.end());
}

void TestPatch() {
	std::printf("the downgrade patch\n");

	std::vector<uint8_t> oldBytes, newBytes, patch, rebuilt;
	std::string          error;
	MakeTwoBuilds(&oldBytes, &newBytes);

	Check(MakePatchBuffers(oldBytes, newBytes, &patch, &error), "a patch is built: " + error);
	Check(ApplyPatchBuffers(oldBytes, patch, &rebuilt, &error), "and applies");
	Check(rebuilt == newBytes, "what comes out is what went in");

	PatchInfo info;
	Check(ReadPatchInfoBuffer(patch, &info, &error), "its header reads back");
	char line[200];
	std::snprintf(line, sizeof(line),
	              "moved addresses cost almost nothing: the patch is %.1f%% of the file (%zu bytes)",
	              100.0 * patch.size() / newBytes.size(), patch.size());
	Check(patch.size() < newBytes.size() / 20, line);
	std::snprintf(line, sizeof(line),
	              "and only the genuinely new block is stored as itself (%u of %u bytes)",
	              info.extraBytes, 0x1230u);
	Check(info.extraBytes <= 0x1230 + 256, line);
	Check(info.oldMd5 == launcher::Md5Buffer(oldBytes.data(), oldBytes.size()) &&
	          info.newMd5 == launcher::Md5Buffer(newBytes.data(), newBytes.size()) &&
	          info.oldSize == oldBytes.size() && info.newSize == newBytes.size(),
	      "the header names both ends by size and MD5");

	std::vector<uint8_t> wrong = oldBytes;
	wrong[12345] ^= 0x01;
	std::vector<uint8_t> nothing;
	Check(!ApplyPatchBuffers(wrong, patch, &nothing, &error),
	      "a file with one byte changed is refused");
	Check(error.find("MD5") != std::string::npos, "and the reason says why");

	std::vector<uint8_t> shorter(oldBytes.begin(), oldBytes.end() - 1);
	Check(!ApplyPatchBuffers(shorter, patch, &nothing, &error), "a file of the wrong size is refused");
	Check(!ApplyPatchBuffers(oldBytes, Noise(200, 9), &nothing, &error),
	      "something that is not a patch at all is refused");

	std::vector<uint8_t> truncated(patch.begin(), patch.begin() + patch.size() / 2);
	Check(!ApplyPatchBuffers(oldBytes, truncated, &nothing, &error), "a half-written patch is refused");

	std::vector<uint8_t> damaged = patch;
	damaged[damaged.size() - 10] ^= 0x55;
	Check(!ApplyPatchBuffers(oldBytes, damaged, &nothing, &error),
	      "a patch with a damaged byte is refused, not applied");

	std::vector<uint8_t> same;
	Check(MakePatchBuffers(oldBytes, oldBytes, &patch, &error) &&
	          ApplyPatchBuffers(oldBytes, patch, &same, &error) && same == oldBytes,
	      "a patch between two identical files works too");

	std::vector<uint8_t> unrelated = Noise(50000, 99), out;
	Check(MakePatchBuffers(oldBytes, unrelated, &patch, &error) &&
	          ApplyPatchBuffers(oldBytes, patch, &out, &error) && out == unrelated,
	      "and between two unrelated ones");

	// The ceiling mkpatch and the stage script hold every patch to.
	PatchInfo unrelatedInfo;
	ReadPatchInfoBuffer(patch, &unrelatedInfo, &error);
	Check(StoredFraction(unrelatedInfo) > kMaxStoredFraction,
	      "a patch that is mostly its own output is over the stored-bytes ceiling");
	Check(StoredFraction(info) < kMaxStoredFraction / 10,
	      "while one between two builds of the same program is far under it");

	// The file level: backup first, then the rename.
	const std::string dir = TempDir("coopiii-installtest-patchfile");
	const std::string exe = launcher::Join(dir, "gta3.exe");
	const std::string bak = launcher::Join(dir, "gta3.exe.bak");
	const std::string pat = launcher::Join(dir, "x.c3patch");
	MakePatchBuffers(oldBytes, newBytes, &patch, &error);
	WriteWhole(exe, oldBytes);
	WriteWhole(pat, patch);
	Check(ApplyPatch(exe, pat, bak, &error), "ApplyPatch works on files");
	Check(ReadWhole(exe) == newBytes && ReadWhole(bak) == oldBytes,
	      "the exe is the new build and the backup is the old one");
	Check(!ApplyPatch(exe, pat, bak, &error) && ReadWhole(bak) == oldBytes,
	      "applying it a second time is refused and does not touch the backup");
	RemoveTree(dir);
}

// ---- the manifest ---------------------------------------------------------

bool Https(const std::string &url) { return url.rfind("https://", 0) == 0; }

void TestManifest() {
	std::printf("the manifest\n");

	std::string     why;
	const Manifest &m = BuiltInManifest(&why);
	Check(why.empty() && !m.components.empty(), "the built-in manifest parses" +
	                                                (why.empty() ? std::string() : ": " + why));

	const Component *coop = m.Find("coopiii");
	Check(coop && coop->required && coop->source == "local" && coop->Ready() &&
	          coop->files.size() == 3,
	      "CoopIII is in it, required, carried by the Setup, three files");

	for (const char *id : {"asiloader", "silentpatch"}) {
		const Component *c = m.Find(id);
		Check(c && c->required && c->Downloadable(), std::string(id) + " is required");
	}

	// Only MIT components may have a copy in CoopIII's release. These three
	// are fetched from their authors and nowhere else.
	for (const char *id : {"sacarcam", "ginput", "cleo"}) {
		const Component *c = m.Find(id);
		Check(c && c->mirrors.empty() && c->url.find("noxxUY") == std::string::npos,
		      std::string(id) + " is only ever fetched from its author");
	}
	if (const Component *cam = m.Find("sacarcam"))
		Check(!cam->required && !cam->selected && cam->saveAs == "SACarCam.asi",
		      "SACarCam is optional, off by default, and lands as an .asi");

	Check(m.FindBuild(launcher::GAME_MD5) != nullptr, "v1.0 retail is a known build");
	Check(m.FindBuild("85414bf9eb414d00ad81062360f0db1f") != nullptr, "looked up in any case");

	// The Steam exe is known by name and has no patch, on purpose: it is
	// wrapped in SteamStub, and a patch from it would be most of the game.
	const Build *steam = m.FindBuild("12BDB699AA2F4922240C454C2E567F3F");
	Check(steam && steam->name == "Steam" && steam->size == 2801664, "the Steam exe is a known build");
	Check(!m.FindPatch("12BDB699AA2F4922240C454C2E567F3F"), "and there is no patch for it");
	Check(m.patches.empty(), "there is no downgrade patch in the manifest at all");
	Check(m.downgrader.url.rfind("https://gtaforums.com/", 0) == 0 &&
	          m.downgrader.infoUrl.rfind("https://www.pcgamingwiki.com/", 0) == 0 &&
	          !m.downgrader.name.empty(),
	      "players of it are sent to a guide and a wiki page, by name");

	bool allReady = true, allHttps = true, zipsHaveRules = true, mirrorsOurs = true,
	     haveLicence = true;
	for (const Component &c : m.components) {
		if (!c.Ready()) {
			allReady = false;
			std::printf("      not ready: %s\n", c.id.c_str());
		}
		for (const std::string &s : c.Sources())
			if (!Https(s))
				allHttps = false;
		for (const std::string &s : c.mirrors)
			if (s.rfind("https://github.com/noxxUY/CoopIII/releases/", 0) != 0)
				mirrorsOurs = false;
		if (c.archive == "zip" && c.extract.empty())
			zipsHaveRules = false;
		if (c.license.empty())
			haveLicence = false;
	}
	Check(allReady, "every component has a download and a hash, so all of them can install");
	Check(allHttps, "every source is https");
	Check(mirrorsOurs, "every mirror is a CoopIII release asset");
	Check(zipsHaveRules, "every zip says which files to take out of it");
	Check(haveLicence, "every component names its licence");

	Manifest    bad;
	std::string error;
	Check(!bad.Parse("", &error), "an empty manifest is refused");
	Check(!bad.Parse("{\"components\": []}", &error), "one with no components is refused");
	Check(!bad.Parse("{\"components\": [{\"name\": \"no id\", \"source\": \"local\"}]}", &error),
	      "a component with no id is refused");
	Check(!bad.Parse("{\"components\": [{\"id\": \"x\", \"name\": \"x\"", &error),
	      "a truncated file is refused");
	Check(!bad.Parse("{\"components\": [{\"id\": \"x\", \"name\": \"X\", \"source\": \"download\","
	                 " \"archive\": \"zip\", \"url\": \"https://a/b.zip\"}]}",
	                 &error),
	      "a zip with no extract rules is refused");
	Check(!bad.Parse("{\"components\": [{\"id\": \"x\", \"name\": \"X\", \"source\": \"local\"},"
	                 " {\"id\": \"x\", \"name\": \"Y\", \"source\": \"local\"}]}",
	                 &error),
	      "an id used twice is refused");
	Check(!bad.Parse("{\"components\": [{\"id\": \"x\", \"name\": \"X\", \"source\": \"local\","
	                 " \"sha256\": \"abc\"}]}",
	                 &error),
	      "a hash that is not a SHA-256 is refused");
	Check(!bad.Parse("{\"patches\": [{\"from\": \"nope\", \"file\": \"a\", \"sha256\": \"b\"}],"
	                 " \"components\": [{\"id\": \"x\", \"name\": \"X\", \"source\": \"local\"}]}",
	                 &error),
	      "a patch whose MD5 is not one is refused");

	const std::string oneComponent =
	    "\"components\": [{\"id\": \"x\", \"name\": \"X\", \"source\": \"local\"}]}";
	for (const char *url : {"https://example.com/gta3.exe", "https://example.com/Downgrader.ZIP",
	                        "https://example.com/d.7z?dl=1", "http://example.com/guide"}) {
		Check(!bad.Parse(std::string("{\"downgrader\": {\"name\": \"x\", \"url\": \"") + url +
		                     "\"}, " + oneComponent,
		                 &error),
		      std::string("a downgrader link that is a file or not https is refused: ") + url);
	}
	Check(bad.Parse("{\"downgrader\": {\"name\": \"x\", \"url\": \"\"}, " + oneComponent, &error) &&
	          bad.downgrader.url.empty(),
	      "a downgrader with no link yet is allowed, and offers none");

	Manifest good;
	Check(good.Parse("{\"note\": \"ignored\", \"components\": [{\"id\": \"a\", \"name\": \"A\","
	                 " \"source\": \"local\", \"required\": true, \"selected\": false,"
	                 " \"somethingNew\": {\"deep\": [1, 2, null]}}]}",
	                 &error),
	      "a manifest with keys this build does not know still parses");
	Check(good.components.size() == 1 && good.components[0].required &&
	          good.components[0].selected,
	      "and a required component is always selected");
}

// ---- archives -------------------------------------------------------------

std::vector<uint8_t> RawZipWithPath(const std::string &name) {
	return WriteZip({{name, Bytes("payload")}});
}

void TestArchive() {
	std::printf("archives\n");

	const std::vector<ArchiveEntry> in = {
	    {"dinput8.dll", Bytes("loader")},
	    {"docs/readme.txt", Bytes("read me")},
	    {"CLEO/CLEO_PLUGINS/a.cleo", Bytes("plugin a")},
	    {"CLEO/b.txt", Bytes("b")},
	    {"scripts/Thing.asi", Bytes("thing")},
	};
	std::vector<ArchiveEntry> out;
	std::string               error;
	Check(ReadZip(WriteZip(in), &out, &error) && out.size() == in.size(),
	      "a zip written here reads back");
	bool same = out.size() == in.size();
	for (size_t i = 0; same && i < in.size(); ++i)
		same = out[i].path == in[i].path && out[i].bytes == in[i].bytes;
	Check(same, "every file, byte for byte");

	Check(!ReadZip(RawZipWithPath("../evil.dll"), &out, &error), "a path that climbs out is refused");
	Check(!ReadZip(RawZipWithPath("a/../../evil.dll"), &out, &error), "even half way down");
	Check(!ReadZip(RawZipWithPath("C:/Windows/evil.dll"), &out, &error), "so is a drive letter");
	Check(!ReadZip(RawZipWithPath("/evil.dll"), &out, &error), "and a rooted path");
	Check(!ReadZip(Noise(1000, 3), &out, &error), "something that is not a zip is refused");

	ReadZip(WriteZip(in), &out, &error);
	std::vector<std::pair<std::string, const ArchiveEntry *>> plan;
	Check(PlanExtract(out,
	                  {{"dinput8.dll", "dinput8.dll"},
	                   {"cleo/", "CLEO/"},
	                   {"scripts/Thing.asi", "Thing.asi"}},
	                  &plan, &error),
	      "extract rules resolve");
	bool shaped = plan.size() == 4 && plan[0].first == "dinput8.dll" &&
	              plan[1].first == "CLEO/CLEO_PLUGINS/a.cleo" && plan[2].first == "CLEO/b.txt" &&
	              plan[3].first == "Thing.asi";
	Check(shaped, "a file, a folder (in any case) and a rename land where they are told");
	bool noDocs = true;
	for (const auto &p : plan)
		if (p.first.find("readme") != std::string::npos)
			noDocs = false;
	Check(noDocs, "and what no rule names is left in the archive");
	Check(!PlanExtract(out, {{"missing.asi", "missing.asi"}}, &plan, &error),
	      "a rule that matches nothing fails, since the archive is not the expected one");
	Check(!PlanExtract(out, {{"dinput8.dll", "../dinput8.dll"}}, &plan, &error),
	      "a rule that points outside the game folder fails");
}

// ---- the install record ---------------------------------------------------

void TestRecord() {
	std::printf("the install record\n");
	const std::string dir = TempDir("coopiii-installtest-record");

	InstallRecord r;
	r.entries.push_back({RecordEntry::Kind::Dir, "CLEO", "", ""});
	r.entries.push_back({RecordEntry::Kind::File, "CLEO\\a.cleo", std::string(64, 'a'), ""});
	r.entries.push_back({RecordEntry::Kind::File, "dinput8.dll", std::string(64, 'b'),
	                     "CoopIII-Setup-backup\\dinput8.dll"});
	r.entries.push_back({RecordEntry::Kind::Exe, "gta3.exe", std::string(32, 'C'), "gta3.exe.bak"});
	std::string error;
	Check(r.Save(dir, &error), "it saves");
	Check(HasInstallRecord(dir), "and is found");

	InstallRecord back;
	Check(back.Load(dir, &error) && back.entries.size() == 4, "it loads");
	bool same = back.entries.size() == 4;
	for (size_t i = 0; same && i < 4; ++i)
		same = back.entries[i].kind == r.entries[i].kind && back.entries[i].path == r.entries[i].path &&
		       back.entries[i].hash == r.entries[i].hash && back.entries[i].backup == r.entries[i].backup;
	Check(same, "every entry comes back as it went");
	Check(back.Find(RecordEntry::Kind::File, "cleo\\A.CLEO") != nullptr, "paths match in any case");

	WriteWhole(launcher::Join(dir, kRecordName), Bytes("something else\r\nfile\tx\ty\tz\r\n"));
	Check(!back.Load(dir, &error), "a file that is not a record is refused");

	InstallRecord empty;
	Check(empty.Save(dir, &error) && !HasInstallRecord(dir), "an empty record deletes the file");
	RemoveTree(dir);
}

// ---- installing, offline ----------------------------------------------------

// A manifest whose downloads are already sitting in the cache, so the whole
// install path runs - hash, unzip, place, back up, record - without a network.
struct Fixture {
	std::string game, cache;
	Manifest    manifest;
	std::vector<uint8_t> loaderZip, modZip;
};

std::string CacheName(const std::string &sha, const std::string &leaf) {
	return sha.substr(0, 12) + "-" + leaf;
}

void BuildFixture(Fixture *f, const char *leaf) {
	f->game  = TempDir(leaf);
	f->cache = launcher::Join(f->game, "..\\coopiii-installtest-cache");
	RemoveTree(f->cache);
	CreateDirectoryA(f->cache.c_str(), nullptr);

	f->loaderZip = WriteZip({{"dinput8.dll", Bytes("the loader")}, {"readme.txt", Bytes("r")}});
	f->modZip    = WriteZip({{"Mod.asi", Bytes("the mod")},
	                         {"Mod.ini", Bytes("[defaults]")},
	                         {"Mod/data/a.dat", Bytes("a")},
	                         {"Mod/data/deep/b.dat", Bytes("b")}});
	const std::string loaderSha = Sha256(f->loaderZip.data(), f->loaderZip.size());
	const std::string modSha    = Sha256(f->modZip.data(), f->modZip.size());
	WriteWhole(launcher::Join(f->cache, CacheName(loaderSha, "loader.zip").c_str()), f->loaderZip);
	WriteWhole(launcher::Join(f->cache, CacheName(modSha, "mod.zip").c_str()), f->modZip);

	const std::string json =
	    "{\"components\": ["
	    "{\"id\": \"coopiii\", \"name\": \"CoopIII\", \"source\": \"local\", \"required\": true,"
	    " \"files\": [\"CoopIII.asi\", \"CoopIII.ini\", \"coopiii-launcher.exe\"]},"
	    "{\"id\": \"loader\", \"name\": \"Loader\", \"source\": \"download\", \"license\": \"MIT\","
	    " \"url\": \"https://example.invalid/loader.zip\", \"sha256\": \"" + loaderSha + "\","
	    " \"archive\": \"zip\", \"extract\": [{\"from\": \"dinput8.dll\", \"to\": \"dinput8.dll\"}],"
	    " \"defaults\": [{\"file\": \"global.ini\", \"to\": \"scripts/global.ini\"}]},"
	    "{\"id\": \"mod\", \"name\": \"Mod\", \"source\": \"download\", \"license\": \"MIT\","
	    " \"url\": \"https://example.invalid/mod.zip\", \"sha256\": \"" + modSha + "\","
	    " \"archive\": \"zip\", \"extract\": [{\"from\": \"Mod.asi\", \"to\": \"Mod.asi\"},"
	    " {\"from\": \"Mod.ini\", \"to\": \"Mod.ini\"}, {\"from\": \"Mod/\", \"to\": \"moddata/\"}]},"
	    "{\"id\": \"plain\", \"name\": \"Insecure\", \"source\": \"download\", \"license\": \"MIT\","
	    " \"url\": \"http://example.invalid/plain.asi\", \"sha256\": \"" + std::string(64, '0') + "\"}"
	    "]}";
	std::string error;
	if (!f->manifest.Parse(json, &error))
		std::printf("      fixture manifest: %s\n", error.c_str());
}

void TestInstallJob() {
	std::printf("installing\n");

	const std::vector<uint8_t> asi = Bytes("C3ASI build 1");
	const std::vector<uint8_t> ini = Bytes("[coopiii]\r\n");
	const std::vector<uint8_t> exe(4096, 0xCC);
	const std::vector<uint8_t> globalIni = Bytes("[GlobalSets]\r\n");
	SetPayloadReader([&](const std::string &name, std::vector<uint8_t> *out) {
		if (name == "CoopIII.asi") { *out = asi; return true; }
		if (name == "CoopIII.ini") { *out = ini; return true; }
		if (name == "coopiii-launcher.exe") { *out = exe; return true; }
		if (name == "global.ini") { *out = globalIni; return true; }
		return false;
	});

	Fixture f;
	BuildFixture(&f, "coopiii-installtest-game");
	const std::string G = f.game;
	auto at = [&](const char *rel) { return launcher::Join(G, rel); };

	// The player's own things, already in the folder.
	const std::vector<uint8_t> theirGame   = Noise(5000, 42);
	const std::vector<uint8_t> theirLoader = Bytes("the player's own dinput8");
	const std::vector<uint8_t> theirIni    = Bytes("[player's settings]");
	WriteWhole(at("gta3.exe"), theirGame);
	WriteWhole(at("dinput8.dll"), theirLoader);
	WriteWhole(at("Mod.ini"), theirIni);
	const std::vector<std::string> before = Tree(G);

	InstallOptions o;
	o.gameDir    = G;
	o.cacheDir   = f.cache;
	o.manifest   = &f.manifest;
	o.components = {"mod", "coopiii", "loader"};   // out of order on purpose

	{
		InstallJob job;
		job.Start(o);
		const Progress p = RunToCompletion(job);
		if (p.failed)
			PrintDetails(p);
		Check(!p.running && !p.failed && p.finished == 3 && p.total == 3, "three components install clean");
		Check(p.steps.size() == 3 && p.steps[0].id == "coopiii" && p.steps[1].id == "loader" &&
		          p.steps[2].id == "mod",
		      "in the manifest's order, not the order they were asked for");
	}

	Check(ReadWhole(at("CoopIII.asi")) == asi && ReadWhole(at("CoopIII.ini")) == ini &&
	          ReadWhole(at("coopiii-launcher.exe")) == exe,
	      "CoopIII's own files landed byte for byte");
	Check(ReadWhole(at("dinput8.dll")) == Bytes("the loader"), "the loader came out of its zip");
	Check(ReadWhole(at("CoopIII-Setup-backup\\dinput8.dll")) == theirLoader,
	      "and the player's own dinput8.dll was backed up, not lost");
	Check(ReadWhole(at("scripts\\global.ini")) == globalIni, "the default global.ini was written");
	Check(!launcher::FileExists(at("readme.txt")), "a file no rule names stayed in the archive");
	Check(ReadWhole(at("Mod.ini")) == theirIni, "an .ini the player already had was kept as it was");
	Check(ReadWhole(at("moddata\\data\\deep\\b.dat")) == Bytes("b"), "a folder rule kept its tree");
	Check(ReadWhole(at("gta3.exe")) == theirGame, "gta3.exe was not touched without a downgrade");

	InstallRecord r;
	std::string   error;
	r.Load(G, &error);
	Check(r.Find(RecordEntry::Kind::File, "dinput8.dll") &&
	          r.Find(RecordEntry::Kind::File, "dinput8.dll")->backup == "CoopIII-Setup-backup\\dinput8.dll",
	      "the record knows where the replaced file went");
	Check(!r.Find(RecordEntry::Kind::File, "Mod.ini"), "and does not claim the player's .ini");
	Check(r.Find(RecordEntry::Kind::Dir, "moddata\\data\\deep") && r.Find(RecordEntry::Kind::Dir, "scripts"),
	      "it lists the folders it created");
	const size_t entries = r.entries.size();

	// Again, over the top: nothing new, nothing doubled, the backup untouched.
	{
		InstallJob job;
		job.Start(o);
		const Progress p = RunToCompletion(job);
		Check(!p.failed && p.finished == 3, "installing a second time works");
	}
	r.Load(G, &error);
	Check(r.entries.size() == entries, "and does not add to the record");
	Check(ReadWhole(at("CoopIII-Setup-backup\\dinput8.dll")) == theirLoader,
	      "or overwrite the backup with the Setup's own file");

	// A cache entry that has been tampered with is not used.
	{
		const std::string modSha = Sha256(f.modZip.data(), f.modZip.size());
		const std::string cached = launcher::Join(f.cache, CacheName(modSha, "mod.zip").c_str());
		WriteWhole(cached, Bytes("not the zip"));
		InstallJob job;
		InstallOptions only = o;
		only.components     = {"mod"};
		job.Start(only);
		const Progress p = RunToCompletion(job);
		Check(p.failed && !launcher::FileExists(cached),
		      "a cached download whose hash is wrong is thrown away, not installed");
		WriteWhole(cached, f.modZip);
	}

	// http is refused before anything is fetched.
	{
		InstallJob job;
		InstallOptions only = o;
		only.components     = {"plain"};
		job.Start(only);
		const Progress p = RunToCompletion(job);
		Check(p.failed && p.steps.size() == 1 && p.steps[0].note.find("https") != std::string::npos,
		      "a source that is not https is refused, and the note says so");
	}

	// Uninstall, with one of the Setup's files changed by the player first.
	WriteWhole(at("Mod.asi"), Bytes("the player edited this"));
	std::vector<std::string> log;
	Check(Uninstall(G, &log, &error), "uninstall succeeds: " + error);
	Check(ReadWhole(at("dinput8.dll")) == theirLoader, "the player's dinput8.dll is back");
	Check(ReadWhole(at("Mod.ini")) == theirIni, "their .ini is untouched");
	Check(ReadWhole(at("Mod.asi")) == Bytes("the player edited this"),
	      "a file changed since the install is left alone");
	bool said = false;
	for (const std::string &line : log)
		if (line.find("Mod.asi") != std::string::npos && line.find("changed") != std::string::npos)
			said = true;
	Check(said, "and the log says so");
	DeleteFileA(at("Mod.asi").c_str());
	Check(Tree(G) == before, "everything else is exactly as it was before the install");
	Check(!HasInstallRecord(G), "and the record is gone");

	// The unknown id and the cancel, which the window relies on.
	{
		InstallJob job;
		InstallOptions only = o;
		only.components     = {"not-a-real-component"};
		job.Start(only);
		const Progress p = RunToCompletion(job);
		Check(!p.running && p.total == 0, "an id the manifest does not have is dropped, not run");
	}
	{
		InstallJob job;
		job.Start(o);
		job.Cancel();
		const Progress p = RunToCompletion(job);
		bool allFinal = true;
		for (const StepProgress &s : p.steps)
			if (s.state == StepState::Waiting || s.state == StepState::Working)
				allFinal = false;
		Check(!p.running && allFinal, "a cancelled job finishes and leaves nothing half-done");
		std::vector<std::string> ignored;
		Uninstall(G, &ignored, &error);
	}

	SetPayloadReader(nullptr);
	RemoveTree(f.cache);
	RemoveTree(G);
}

// ---- the downgrade, end to end ----------------------------------------------

// Stands a different build up from the real v1.0 exe: a block inserted in
// the middle and a few bytes changed elsewhere. Close enough to a second
// compile to exercise everything the Setup does with a real gta3.exe.
void TestDowngrade() {
	std::printf("the downgrade, on a real v1.0 gta3.exe\n");

	const char *path = std::getenv("COOPIII_GTA3_EXE");
	if (!path || !launcher::FileExists(path)) {
		std::printf("  (COOPIII_GTA3_EXE is not set; skipped)\n");
		return;
	}
	const std::vector<uint8_t> v10 = ReadWhole(path);
	if (launcher::Md5Buffer(v10.data(), v10.size()) != launcher::GAME_MD5) {
		Check(false, "COOPIII_GTA3_EXE is v1.0 retail");
		return;
	}

	std::vector<uint8_t> other = v10;
	const std::vector<uint8_t> block = Noise(8192, 5);
	other.insert(other.begin() + 0x100000, block.begin(), block.end());
	for (size_t at = 0x1000; at < 0x2000; at += 64)
		other[at] ^= 0x5A;
	const std::string otherMd5 = launcher::Md5Buffer(other.data(), other.size());

	std::vector<uint8_t> patch;
	std::string          error;
	Check(MakePatchBuffers(other, v10, &patch, &error), "a patch to v1.0 is built");
	PatchInfo info;
	ReadPatchInfoBuffer(patch, &info, &error);
	char line[160];
	std::snprintf(line, sizeof(line), "it is %zu bytes, and stores %u bytes of v1.0 as themselves",
	              patch.size(), info.extraBytes);
	Check(info.extraBytes < 1024, line);

	Fixture f;
	BuildFixture(&f, "coopiii-installtest-downgrade");
	const std::string patchSha = Sha256(patch.data(), patch.size());
	WriteWhole(launcher::Join(f.cache, "test.c3patch"), patch);
	PatchSource source;
	source.from   = otherMd5;
	source.name   = "Test build";
	source.file   = "test.c3patch";
	source.url    = "https://example.invalid/test.c3patch";
	source.sha256 = patchSha;
	f.manifest.patches.push_back(source);
	f.manifest.builds.push_back({otherMd5, other.size(), "Test build"});

	const std::string G = f.game;
	WriteWhole(launcher::Join(G, "gta3.exe"), other);
	const std::vector<std::string> before = Tree(G);

	const GameExe g = InspectGame(G, f.manifest);
	Check(g.present && !g.isV10 && g.buildName == "Test build" && g.knownPatch && g.CanDowngrade(),
	      "the Setup recognises the build and knows a patch for it");

	SetPayloadReader([&](const std::string &name, std::vector<uint8_t> *out) {
		*out = Bytes("payload " + name);
		return true;
	});
	InstallOptions o;
	o.gameDir    = G;
	o.cacheDir   = f.cache;
	o.manifest   = &f.manifest;
	o.downgrade  = true;
	o.components = {"coopiii"};
	{
		InstallJob job;
		job.Start(o);
		const Progress p = RunToCompletion(job, 60000);
		if (p.failed)
			PrintDetails(p);
		Check(!p.failed && p.finished == 2 && p.steps[0].id == kDowngradeId,
		      "the downgrade runs first, then the components");
		Check(p.downgraded && p.backup == "gta3.exe.bak", "and reports where the original went");
	}
	Check(launcher::Md5File(launcher::Join(G, "gta3.exe")) == launcher::GAME_MD5,
	      "gta3.exe is now v1.0 retail");
	Check(ReadWhole(launcher::Join(G, "gta3.exe.bak")) == other, "and the original is gta3.exe.bak");
	Check(InspectGame(G, f.manifest).isV10, "which the Setup now sees");

	// A second downgrade has nothing to do.
	{
		InstallJob job;
		job.Start(o);
		const Progress p = RunToCompletion(job, 60000);
		Check(!p.failed && !p.downgraded, "downgrading a v1.0 exe again does nothing");
	}

	std::vector<std::string> log;
	Check(Uninstall(G, &log, &error), "uninstall succeeds: " + error);
	Check(ReadWhole(launcher::Join(G, "gta3.exe")) == other, "and puts the original gta3.exe back");
	Check(Tree(G) == before, "leaving the folder as it was found");

	// An exe nobody has a patch for.
	WriteWhole(launcher::Join(G, "gta3.exe"), Noise(9000, 77));
	{
		InstallJob job;
		job.Start(o);
		const Progress p = RunToCompletion(job);
		Check(p.failed && p.steps[0].state == StepState::Failed &&
		          p.steps[0].note.find("no downgrade patch") != std::string::npos,
		      "an unknown build is refused with a reason that names its MD5");
		Check(p.steps.size() == 2 && p.steps[1].state == StepState::Skipped,
		      "and nothing else is installed on top of it");
	}

	SetPayloadReader(nullptr);
	RemoveTree(f.cache);
	RemoveTree(G);
}

// ---- a known build with no patch --------------------------------------------

// What a Steam player meets: the Setup names the build, offers no downgrade,
// and if asked to install anyway leaves gta3.exe byte for byte as it was.
void TestKnownBuildWithoutPatch() {
	std::printf("a known build the Setup has no patch for\n");

	Fixture f;
	BuildFixture(&f, "coopiii-installtest-nopatch");
	const std::vector<uint8_t> exe = Noise(7000, 31);
	const std::string          md5 = launcher::Md5Buffer(exe.data(), exe.size());
	f.manifest.builds.push_back({md5, exe.size(), "Steam"});
	WriteWhole(launcher::Join(f.game, "gta3.exe"), exe);

	const GameExe g = InspectGame(f.game, f.manifest);
	Check(g.present && !g.isV10 && g.buildName == "Steam", "it is recognised by name");
	Check(!g.CanDowngrade() && g.localPatch.empty() && !g.knownPatch,
	      "and the Setup does not offer to downgrade it");

	SetPayloadReader([](const std::string &name, std::vector<uint8_t> *out) {
		*out = Bytes("payload " + name);
		return true;
	});
	InstallOptions o;
	o.gameDir    = f.game;
	o.cacheDir   = f.cache;
	o.manifest   = &f.manifest;
	o.downgrade  = true;
	o.components = {"coopiii"};
	InstallJob job;
	job.Start(o);
	const Progress p = RunToCompletion(job);
	Check(p.failed && !p.downgraded && p.steps.size() == 2 &&
	          p.steps[1].state == StepState::Skipped,
	      "an install asked for anyway stops at the downgrade and installs nothing");
	Check(ReadWhole(launcher::Join(f.game, "gta3.exe")) == exe &&
	          !launcher::FileExists(launcher::Join(f.game, "gta3.exe.bak")) &&
	          !launcher::FileExists(launcher::Join(f.game, "CoopIII.asi")),
	      "gta3.exe is untouched, no backup was made, no mod was written");

	SetPayloadReader(nullptr);
	RemoveTree(f.cache);
	RemoveTree(f.game);
}

// ---- online ---------------------------------------------------------------

int Online(const std::string &root) {
	std::printf("the real Essential Pack, from the internet\n");

	const char *path = std::getenv("COOPIII_GTA3_EXE");
	if (!path || launcher::Md5File(path) != launcher::GAME_MD5) {
		std::printf("  COOPIII_GTA3_EXE has to name a v1.0 gta3.exe\n");
		return 2;
	}

	EnsureDir(root);
	const std::string game  = launcher::Join(root, "game");
	const std::string cache = launcher::Join(root, "cache");
	RemoveTree(game);
	RemoveTree(cache);
	EnsureDir(game);
	WriteWhole(launcher::Join(game, "gta3.exe"), ReadWhole(path));
	const std::vector<std::string> before = Tree(game);

	SetPayloadReader([](const std::string &name, std::vector<uint8_t> *out) {
		const std::string here = launcher::Join(launcher::ExeDir(), name.c_str());
		if (launcher::FileExists(here))
			return ReadWholeFile(here, out, nullptr);
		*out = Bytes("stand-in for " + name);
		return true;
	});

	const Manifest &m = BuiltInManifest();
	InstallOptions  o;
	o.gameDir  = game;
	o.cacheDir = cache;
	for (const Component &c : m.components)
		o.components.push_back(c.id);

	// Framerate Vigilante has only the CoopIII mirror as a source, and that
	// does not exist until the first release; everything else is upstream.
	InstallJob job;
	job.Start(o);
	const Progress p = RunToCompletion(job, 600000);
	PrintDetails(p);
	for (const StepProgress &s : p.steps)
		Check(s.state == StepState::Done || s.id == "vigilante",
		      s.name + ": " + (s.state == StepState::Done ? "installed" : s.note));

	std::printf("  the game folder now holds:\n");
	for (const std::string &file : Tree(game))
		std::printf("      %s\n", file.c_str());

	// A mirror is only used when the first source fails.
	Manifest    fallback;
	std::string error;
	const Component *sp = m.Find("silentpatch");
	const std::string json =
	    "{\"components\": [{\"id\": \"sp\", \"name\": \"SilentPatch via a fallback\","
	    " \"source\": \"download\", \"license\": \"MIT\","
	    " \"url\": \"https://github.com/noxxUY/CoopIII/releases/download/no-such-tag/nothing.zip\","
	    " \"mirrors\": [\"" + sp->url + "\"], \"sha256\": \"" + sp->sha256 + "\","
	    " \"archive\": \"zip\", \"extract\": [{\"from\": \"SilentPatchIII.asi\", \"to\": \"x/SilentPatchIII.asi\"}]}]}";
	Check(fallback.Parse(json, &error), "a manifest with a dead first source parses");
	const std::string cache2 = launcher::Join(root, "cache2");
	RemoveTree(cache2);
	InstallOptions o2;
	o2.gameDir    = game;
	o2.cacheDir   = cache2;
	o2.manifest   = &fallback;
	o2.components = {"sp"};
	InstallJob job2;
	job2.Start(o2);
	const Progress p2 = RunToCompletion(job2, 120000);
	PrintDetails(p2);
	Check(!p2.failed, "a dead first source falls through to the next one");

	std::vector<std::string> log;
	Check(Uninstall(game, &log, &error), "uninstall succeeds: " + error);
	for (const std::string &line : log)
		std::printf("      %s\n", line.c_str());
	Check(Tree(game) == before, "and the scratch game folder is back to just gta3.exe");

	SetPayloadReader(nullptr);
	return g_failures == 0 ? 0 : 1;
}

int Measure(const std::string &a, const std::string &b) {
	std::vector<uint8_t> oldBytes = ReadWhole(a), newBytes = ReadWhole(b), patch;
	std::string          error;
	const auto           start = std::chrono::steady_clock::now();
	if (!MakePatchBuffers(oldBytes, newBytes, &patch, &error)) {
		std::printf("%s\n", error.c_str());
		return 1;
	}
	const double seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	PatchInfo info;
	ReadPatchInfoBuffer(patch, &info, &error);
	std::printf("%zu -> %zu bytes: patch %zu bytes (%.1f%%), %u stored as themselves (%.1f%%), "
	            "%u controls, %.1f s\n",
	            oldBytes.size(), newBytes.size(), patch.size(), 100.0 * patch.size() / newBytes.size(),
	            info.extraBytes, 100.0 * info.extraBytes / newBytes.size(), info.controls, seconds);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc == 4 && std::strcmp(argv[1], "--measure") == 0)
		return Measure(argv[2], argv[3]);

	TestSha256();
	TestPatch();
	TestManifest();
	TestArchive();
	TestRecord();
	TestInstallJob();
	TestDowngrade();
	TestKnownBuildWithoutPatch();

	if (argc == 3 && std::strcmp(argv[1], "--online") == 0)
		Online(argv[2]);

	std::printf("\n%s\n",
	            g_failures == 0 ? "all installer checks passed" : "installer checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
