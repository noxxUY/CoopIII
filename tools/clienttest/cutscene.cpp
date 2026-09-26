// A cutscene's animations and the models they are laid over (game/cutscene.h):
// finding the scene in anim\cuts.dir, reading its animations' names the way
// LoadAnimFile reads them, and what the owner asks everybody for ahead of it.
// With a retail exe, the bytes that fault when a model is missing; with the
// game's own anim folder beside it, every scene it ships.

#include "game/addresses.h"
#include "game/cutscene.h"
#include "game/replay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_sceneFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_sceneFailures;
}

void Put32(std::vector<uint8_t> &b, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutChunk(std::vector<uint8_t> &b, const char *id, const std::vector<uint8_t> &payload) {
	b.insert(b.end(), id, id + 4);
	Put32(b, static_cast<uint32_t>(payload.size()));
	b.insert(b.end(), payload.begin(), payload.end());
	while (b.size() % 4 != 0)
		b.push_back(0xDC);   // what the stock files pad with
}

std::vector<uint8_t> Text(const char *s) {
	std::vector<uint8_t> v(s, s + std::strlen(s));
	v.push_back(0);
	return v;
}

// An .IFP laid out as l1_lg.ifp is: INFO's ten bytes padded to twelve, names
// of odd lengths, and animation data whose size is not a multiple of four.
std::vector<uint8_t> Ifp(const std::vector<const char *> &names, uint32_t count) {
	std::vector<uint8_t> info;
	Put32(info, count);
	const std::vector<uint8_t> name = Text("l1_lg");
	info.insert(info.end(), name.begin(), name.end());
	std::vector<uint8_t> body;
	PutChunk(body, "INFO", info);
	uint32_t data = 0x1A6D;
	for (const char *n : names) {
		PutChunk(body, "NAME", Text(n));
		PutChunk(body, "DGAN", std::vector<uint8_t>(data++, 0x11));
	}
	std::vector<uint8_t> ifp;
	PutChunk(ifp, "ANPK", body);
	return ifp;
}

void TestTheScenesAnimationsAreRead() {
	std::printf("\na cutscene's animations, as LoadAnimFile reads them\n");
	const std::vector<uint8_t> ifp = Ifp({"eight2", "ludoor", "luigi", "micky", "player"}, 5);
	CutsceneAnims a;
	Check(ReadIfpAnimNames(ifp.data(), ifp.size(), &a) && a.count == 5,
	      "Luigi's scene has five animations");
	Check(std::strcmp(a.names[0], "eight2") == 0 && std::strcmp(a.names[4], "player") == 0,
	      "numbered in file order, 'eight2' first: the one the crash was on");
	Check(!ReadIfpAnimNames(ifp.data(), ifp.size() - 100, &a) && a.count == 0,
	      "a file cut short reads as nothing, not as half a scene");
	const std::vector<uint8_t> lying = Ifp({"eight2"}, 2);
	Check(!ReadIfpAnimNames(lying.data(), lying.size(), &a), "nor does one that counts more than it has");
	const std::vector<uint8_t> huge = Ifp({"a"}, CUTSCENE_ANIMS_MAX + 1);
	Check(!ReadIfpAnimNames(huge.data(), huge.size(), &a), "nor one with more than there is room for");
	std::vector<uint8_t> wrong = ifp;
	std::memcpy(wrong.data(), "ANPX", 4);
	Check(!ReadIfpAnimNames(wrong.data(), wrong.size(), &a), "nor something that is not an .IFP");
	const std::vector<uint8_t> longName = Ifp({"a_name_well_over_twenty_four"}, 1);
	Check(ReadIfpAnimNames(longName.data(), longName.size(), &a) &&
	          std::strlen(a.names[0]) == ANIM_NAME_LEN - 1,
	      "and a name longer than the engine keeps is cut where it cuts it");
}

void TestTheSceneIsFoundInTheDirectory() {
	std::printf("\nfinding it in anim\\cuts.dir\n");
	std::vector<uint8_t> dir(3 * CUTS_DIR_ENTRY, 0);
	auto entry = [&](size_t i, uint32_t off, uint32_t size, const char *name) {
		std::memcpy(&dir[i * CUTS_DIR_ENTRY], &off, 4);
		std::memcpy(&dir[i * CUTS_DIR_ENTRY + 4], &size, 4);
		std::memcpy(&dir[i * CUTS_DIR_ENTRY + 8], name, std::strlen(name));
	};
	entry(0, 10, 3, "l1_lg.dat");
	entry(1, 5655, 566, "l1_lg.ifp");
	entry(2, 7000, 9, "l1_lg.ifpx");
	uint32_t off = 0, size = 0;
	Check(FindInCutsDir(dir.data(), dir.size(), "L1_LG.IFP", &off, &size) && off == 5655 && size == 566,
	      "by name, without case, as CDirectory::FindItem finds it");
	Check(!FindInCutsDir(dir.data(), dir.size(), "L1_LG", &off, &size) &&
	          !FindInCutsDir(dir.data(), dir.size(), "L1_LG.IF", &off, &size),
	      "and only the whole name");
	char name[9];
	const uint8_t load[10] = {0xE4, 0x02, 'L', '1', '_', 'L', 'G', 0, 0, 0};
	Check(CutsceneNameOf(load, sizeof load, name) && std::strcmp(name, "L1_LG") == 0,
	      "the scene is the eight bytes of the owner's LOAD_CUTSCENE");
	Check(!CutsceneNameOf(load, 9, name), "which has to have all eight");
}

void TestTheModelsGoAheadOfTheScene() {
	std::printf("\nthe models the owner's scene is laid over, asked for ahead of it\n");
	using namespace game::replay;
	uint8_t code[CUTSCENE_MODEL_CODE];
	int32_t v = 0;

	size_t n = CutsceneModelCode(26, "eight2", code);
	Check(n == CUTSCENE_MODEL_CODE && code[0] == 0x3C && code[1] == 0x02 && LiteralAt(code, n, 0, &v) &&
	          v == 1 && std::memcmp(code + 7, "eight2\0\0", 8) == 0,
	      "8-Ball in his suit is LOAD_SPECIAL_CHARACTER 1 'eight2', as the replay list reads it");
	Check(Find(0x023C) && Find(0x023C)->count == 2 &&
	          2 + EncodedSize(Find(0x023C)->args[0]) + EncodedSize(Find(0x023C)->args[1]) == n,
	      "and is exactly as long as the list says");
	n = CutsceneModelCode(28, "LUIGI", code);
	Check(n == CUTSCENE_MODEL_CODE && LiteralAt(code, n, 0, &v) && v == 3 &&
	          std::memcmp(code + 7, "luigi\0\0\0", 8) == 0,
	      "special03 is character 3, its name lowered as the handler lowers it");
	n = CutsceneModelCode(185, "ludoor", code);
	Check(n == CUTSCENE_MODEL_CODE && code[0] == 0xF3 && code[1] == 0x02 && LiteralAt(code, n, 0, &v) &&
	          v == 185,
	      "a cutscene object is LOAD_SPECIAL_MODEL under its id");
	n = CutsceneModelCode(1234, "luigiineerclub", code);
	Check(n == 7 && code[0] == 0x47 && code[1] == 0x02 && LiteralAt(code, n, 0, &v) && v == 1234,
	      "any other model is only requested: its name is its own everywhere");
	n = CutsceneModelCode(101, "GANG07", code);
	Check(n == 7 && code[0] == 0x47 && LiteralAt(code, n, 0, &v) && v == 101,
	      "a gang member the scene is laid over too, whatever case its name has, so nothing renames it");
	Check(CutsceneModelCode(190, "note", code) == 7 && CutsceneModelCode(184, "note", code) == 7,
	      "and only the five props, 185 to 189, are asked for by name");
	Check(CutsceneModelCode(189, "abcdefgh", code) == 0,
	      "a prop whose name fills all eight bytes is not sent");
	Check(CutsceneModelCode(0, "player", code) == 0 && CutsceneModelCode(0, "playerp", code) == 0,
	      "model 0 is nobody's to ask for: it is each player's own clothes");
	Check(CutsceneModelCode(29, "player", code) == 0 && CutsceneModelCode(29, "playerx", code) == 0,
	      "and neither is a special slot holding one of Claude's outfits");
	Check(CutsceneModelCode(27, "abcdefgh", code) == 0,
	      "a special character's name that fills all eight bytes is not sent");
	Check(CutsceneModelCode(27, "", code) == 0 && CutsceneModelCode(27, nullptr, code) == 0,
	      "nor one with no name");

	int32_t model = -1;
	char    wanted[9];
	n = CutsceneModelCode(26, "eight2", code);
	Check(SpecialLoadTarget(code, n, &model, wanted) && model == 26 && std::strcmp(wanted, "eight2") == 0,
	      "a participant reads LOAD_SPECIAL_CHARACTER 1 back as special01 under 'eight2'");
	const uint8_t shouted[CUTSCENE_MODEL_CODE] = {0xF3, 0x02, 0x01, 185, 0, 0, 0, 'L', 'U', 'D', 'O', 'O', 'R', 0, 0};
	Check(SpecialLoadTarget(shouted, sizeof shouted, &model, wanted) && model == 185 &&
	          std::strcmp(wanted, "ludoor") == 0,
	      "and a script's LOAD_SPECIAL_MODEL #CUTOBJ01 'LUDOOR' as 185 under 'ludoor'");
	const uint8_t fifth[CUTSCENE_MODEL_CODE] = {0x3C, 0x02, 0x01, 5, 0, 0, 0, 'x', 0, 0, 0, 0, 0, 0, 0};
	Check(!SpecialLoadTarget(fifth, sizeof fifth, &model, wanted) &&
	          !SpecialLoadTarget(code, 7, &model, wanted),
	      "there is no fifth special character, and a short instruction is nothing");
	n = CutsceneModelCode(1234, "luigiineerclub", code);
	Check(!SpecialLoadTarget(code, n, &model, wanted), "and REQUEST_MODEL renames nothing");
	Check(IsRename("eight", "eight2") && !IsRename("eight2", "eight2") && IsRename(nullptr, "eight2"),
	      "'eight' asked for as 'eight2' is a rename; the same name is only a request");

	Check(IsCutsceneStep(0x02E5) && IsCutsceneStep(0x02E6) && IsCutsceneStep(0x02E7) &&
	          IsCutsceneStep(0x02F4) && IsCutsceneStep(0x02F5) && IsCutsceneStep(0x0244),
	      "a skipped scene's objects, heads, animations, offset and start are skipped with it");
	Check(!IsCutsceneStep(0x02E4) && !IsCutsceneStep(0x02EA) && !IsCutsceneStep(0x016A),
	      "but not the next scene's load, its end, or the fade around it");
}

// ---- the retail exe --------------------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return va >= IMAGE_BASE && o < img.size() ? img[o] : 0;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	return uint32_t(Byte(img, va)) | uint32_t(Byte(img, va + 1)) << 8 | uint32_t(Byte(img, va + 2)) << 16 |
	       uint32_t(Byte(img, va + 3)) << 24;
}

bool Calls(const std::vector<uint8_t> &img, uint32_t at, uint32_t target) {
	return Byte(img, at) == 0xE8 && at + 5 + Dword(img, at + 1) == target;
}

bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> bytes) {
	for (uint8_t b : bytes)
		if (Byte(img, va++) != b)
			return false;
	return true;
}

bool StringAt(const std::vector<uint8_t> &img, uint32_t va, const char *s) {
	for (size_t i = 0; i <= std::strlen(s); ++i)
		if (Byte(img, va + static_cast<uint32_t>(i)) != static_cast<uint8_t>(s[i]))
			return false;
	return true;
}

void TestTheLoadAgainstTheImage(std::string &exePath) {
	std::printf("\nLOAD_CUTSCENE in gta3.exe\n");
	std::vector<uint8_t> img;
	if (!LoadExe(img, exePath)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "bytes against one\n");
		exePath.clear();
		return;
	}
	std::printf("  reading %s\n", exePath.c_str());
	Check(Dword(img, 0x005EF4D0 + 40 * 4) == 0x00446D52 && Calls(img, 0x00446D79, CCutsceneMgr__LoadCutsceneData),
	      "02E4's handler, 700-table entry 40, calls LoadCutsceneData at 0x00446D79");
	Check(StringAt(img, 0x005EBE7C, "ANIM\\CUTS.DIR") && StringAt(img, 0x005EBE94, "ANIM\\CUTS.IMG") &&
	          StringAt(img, 0x005EBEA4, "%s.IFP"),
	      "which reads the scene out of ANIM\\CUTS.DIR and ANIM\\CUTS.IMG as %s.IFP");
	Check(BytesAt(img, 0x00404729, {0xB9, 0x58, 0x9C, 0x70, 0x00, 0x57}) &&
	          Dword(img, 0x0040472A) == CCutsceneMgr__ms_cutsceneAssociations &&
	          Calls(img, 0x0040472F, CAnimBlendAssocGroup__CreateAssociations),
	      "and hands its name to ms_cutsceneAssociations.CreateAssociations");
	Check(BytesAt(img, 0x0040119E, {0x6B, 0xED, 0x28}) && Dword(img, 0x004011A3) == CAnimManager__ms_aAnimations &&
	          Calls(img, CREATE_ASSOCIATIONS_MODEL_CALL, AnimBlend__GetModelFromName),
	      "which asks GetModelFromName for each animation by its name");
	Check(BytesAt(img, 0x004011AD, {0x89, 0xC7}) && BytesAt(img, 0x004011BF, {0x89, 0xF9}) &&
	          BytesAt(img, CREATE_ASSOCIATIONS_FAULT, {0x8B, 0x31}),
	      "and reads through what it got with nothing between: the fault at 0x004011C4");
	Check(Dword(img, 0x004010E3) == CModelInfo__ms_modelInfoPtrs && BytesAt(img, 0x00401121, {0x81, 0xFD}) &&
	          Dword(img, 0x00401123) == MODELINFO_SIZE && BytesAt(img, 0x004010FD, {0x80, 0x38, 0x02}) &&
	          BytesAt(img, 0x00401129, {0x31, 0xC0}),
	      "GetModelFromName walks every model info for a loaded clump and ends on 0 without one");
	Check(Dword(img, 0x005EFA14 + 7 * 4) == LOAD_ALL_MODELS_NOW_HANDLER &&
	          Calls(img, LOAD_ALL_MODELS_NOW_HANDLER, CTimer__Stop) &&
	          BytesAt(img, LOAD_ALL_MODELS_NOW_HANDLER + 5, {0x6A, 0x00}) &&
	          Calls(img, LOAD_ALL_MODELS_NOW_HANDLER + 7, CStreaming__LoadAllRequestedModels) &&
	          Calls(img, LOAD_ALL_MODELS_NOW_HANDLER + 13, CTimer__Update),
	      "LOAD_ALL_MODELS_NOW is CTimer::Stop, LoadAllRequestedModels(0), CTimer::Update");
	Check(Dword(img, 0x005EF4D0 + 41 * 4) == 0x00446D8E && Calls(img, 0x00446DA1, 0x00404BE0) &&
	          BytesAt(img, 0x00404BE9, {0x81, 0xFB}) && Dword(img, 0x00404BEB) == CUTSCENE_MI_CUTOBJ01 &&
	          BytesAt(img, 0x00404BF5, {0x81, 0xFB}) &&
	          Dword(img, 0x00404BF7) == CUTSCENE_MI_CUTOBJ01 + CUTSCENE_CUTOBJS - 1,
	      "CREATE_CUTSCENE_OBJECT's CreateCutsceneObject takes 185..189 as the props");
	Check(BytesAt(img, 0x00404C1D, {0x8B, 0x0C, 0x9D}) && BytesAt(img, 0x00404C26, {0xFF, 0x55, 0x14}) &&
	          Calls(img, 0x00404C3C, 0x0059EDD0) && BytesAt(img, 0x00404D25, {0x8B, 0x5A, 0x4C}),
	      "and walks a prop's clump without a test, as SET_CUTSCENE_ANIM reads the object's");
	Check(Dword(img, 0x005EF298 + 72 * 4) == 0x00444274 && BytesAt(img, 0x004442CF, {0x6A, 0x06}) &&
	          Calls(img, 0x004442D3, CStreaming__RequestSpecialChar) &&
	          Dword(img, 0x005EF4D0 + 55 * 4) == 0x00447468 && BytesAt(img, 0x004474C0, {0x6A, 0x06}) &&
	          Calls(img, 0x004474C4, CStreaming__RequestSpecialModel),
	      "LOAD_SPECIAL_CHARACTER and LOAD_SPECIAL_MODEL ask for their model script-owned (6)");
}

// ---- the game's own scenes ---------------------------------------------------------

bool ReadAll(const std::string &path, std::vector<uint8_t> &out) {
	FILE *fh = std::fopen(path.c_str(), "rb");
	if (!fh)
		return false;
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	out.resize(size > 0 ? size_t(size) : 0);
	const bool ok = !out.empty() && std::fread(out.data(), 1, out.size(), fh) == out.size();
	std::fclose(fh);
	return ok;
}

void TestEveryStockScene(const std::string &exePath) {
	std::printf("\nevery scene in the game's anim\\cuts.img\n");
	const size_t slash = exePath.find_last_of("\\/");
	const std::string root = slash == std::string::npos ? std::string() : exePath.substr(0, slash + 1);
	std::vector<uint8_t> dir;
	FILE *img = root.empty() || !ReadAll(root + "anim/cuts.dir", dir)
	                ? nullptr
	                : std::fopen((root + "anim/cuts.img").c_str(), "rb");
	if (!img) {
		std::printf("  [skipped] no anim\\cuts.dir and cuts.img beside COOPIII_GTA3_EXE\n");
		return;
	}
	size_t scenes = 0, read = 0;
	bool   luigi  = false;
	for (size_t at = 0; at + CUTS_DIR_ENTRY <= dir.size(); at += CUTS_DIR_ENTRY) {
		char name[25] = {};
		std::memcpy(name, &dir[at + 8], 24);
		const size_t len = std::strlen(name);
		if (len < 4 || (std::strcmp(name + len - 4, ".ifp") != 0 && std::strcmp(name + len - 4, ".IFP") != 0))
			continue;
		++scenes;
		uint32_t off = 0, sectors = 0;
		if (!FindInCutsDir(dir.data(), dir.size(), name, &off, &sectors))
			continue;
		std::vector<uint8_t> ifp(size_t(sectors) * CUTS_SECTOR);
		if (std::fseek(img, long(off) * long(CUTS_SECTOR), SEEK_SET) != 0 ||
		    std::fread(ifp.data(), 1, ifp.size(), img) != ifp.size())
			continue;
		CutsceneAnims a;
		if (!ReadIfpAnimNames(ifp.data(), ifp.size(), &a) || a.count == 0)
			continue;
		++read;
		if (std::strcmp(name, "l1_lg.ifp") == 0)
			luigi = a.count == 5 && std::strcmp(a.names[0], "eight2") == 0 &&
			        std::strcmp(a.names[1], "ludoor") == 0 && std::strcmp(a.names[2], "luigi") == 0 &&
			        std::strcmp(a.names[3], "micky") == 0 && std::strcmp(a.names[4], "player") == 0;
	}
	std::fclose(img);
	char what[96];
	std::snprintf(what, sizeof what, "all %u scenes read (%u)", unsigned(scenes), unsigned(read));
	Check(scenes > 0 && read == scenes, what);
	Check(luigi, "l1_lg.ifp is eight2, ludoor, luigi, micky, player, in that order");
}

} // namespace

int RunCutsceneTests() {
	TestTheScenesAnimationsAreRead();
	TestTheSceneIsFoundInTheDirectory();
	TestTheModelsGoAheadOfTheScene();
	std::string exePath;
	TestTheLoadAgainstTheImage(exePath);
	if (!exePath.empty())
		TestEveryStockScene(exePath);
	return g_sceneFailures;
}
