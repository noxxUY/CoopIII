#include "cutscene.h"

#include "addresses.h"

#include <cstdio>
#include <vector>

namespace coopiii::game {

namespace {

using ModelFromNameFn = void *(__cdecl *)(const char *);
using VoidFn          = void(__cdecl *)();
using LoadAllFn       = void(__cdecl *)(int32_t onlyPriority);

// Relative to the game's own directory, as LoadCutsceneData opens them.
constexpr const char *CUTS_DIR_PATH = "ANIM\\CUTS.DIR";
constexpr const char *CUTS_IMG_PATH = "ANIM\\CUTS.IMG";

bool ReadFileSpan(const char *path, long offset, size_t size, std::vector<uint8_t> &out) {
	FILE *fh = std::fopen(path, "rb");
	if (!fh)
		return false;
	bool ok = true;
	if (size == 0) {
		ok = std::fseek(fh, 0, SEEK_END) == 0;
		const long end = ok ? std::ftell(fh) : -1;
		ok             = ok && end > 0 && std::fseek(fh, 0, SEEK_SET) == 0;
		size           = ok ? static_cast<size_t>(end) : 0;
	} else {
		ok = std::fseek(fh, offset, SEEK_SET) == 0;
	}
	out.resize(size);
	ok = ok && size > 0 && std::fread(out.data(), 1, size, fh) == size;
	std::fclose(fh);
	return ok;
}

bool ReadCutsceneAnims(const char *cutscene, CutsceneAnims *out) {
	std::vector<uint8_t> dir;
	if (!ReadFileSpan(CUTS_DIR_PATH, 0, 0, dir))
		return false;
	char file[24];
	std::snprintf(file, sizeof file, "%s.IFP", cutscene);
	uint32_t offset = 0, sectors = 0;
	if (!FindInCutsDir(dir.data(), dir.size(), file, &offset, &sectors) || sectors == 0 ||
	    sectors > 0x4000)
		return false;
	std::vector<uint8_t> ifp;
	if (!ReadFileSpan(CUTS_IMG_PATH, static_cast<long>(offset) * static_cast<long>(CUTS_SECTOR),
	                  static_cast<size_t>(sectors) * CUTS_SECTOR, ifp))
		return false;
	return ReadIfpAnimNames(ifp.data(), ifp.size(), out);
}

// The model GetModelFromName finds for an animation of this name, as its id,
// or -1 for none: exactly the lookup CreateAssociations makes.
int32_t ModelFor(const char *anim) {
	void *const mi = Func<ModelFromNameFn>(AnimBlend__GetModelFromName)(anim);
	if (!mi)
		return -1;
	void *const *const infos = Ptr<void *>(CModelInfo__ms_modelInfoPtrs);
	for (uint32_t id = 0; id < MODELINFO_SIZE; ++id)
		if (infos[id] == mi)
			return static_cast<int32_t>(id);
	return -1;
}

size_t Resolve(CutsceneModels *m) {
	m->missing = 0;
	for (size_t i = 0; i < m->anims.count; ++i) {
		m->model[i] = ModelFor(m->anims.names[i]);
		if (m->model[i] < 0)
			++m->missing;
	}
	return m->missing;
}

uint8_t StreamingFlags(int32_t id) {
	return *Ptr<uint8_t>(CStreaming__ms_aInfoForModel + static_cast<uint32_t>(id) * STREAMING_INFO_STRIDE +
	                     STREAMING_FLAGS_OFFS);
}

} // namespace

const char *NameOfModel(int32_t id) {
	if (id < 0 || static_cast<uint32_t>(id) >= MODELINFO_SIZE)
		return nullptr;
	const uint8_t *const mi = Ptr<const uint8_t *const>(CModelInfo__ms_modelInfoPtrs)[id];
	return mi ? reinterpret_cast<const char *>(mi + offs::MODELINFO_NAME) : nullptr;
}

bool ScriptOwnsModel(int32_t id) {
	return id >= 0 && static_cast<uint32_t>(id) < MODELINFO_SIZE &&
	       (StreamingFlags(id) & STREAMFLAGS_SCRIPTOWNED) != 0;
}

uint16_t ModelRefs(int32_t id) {
	const char *const name = NameOfModel(id);
	if (!name)
		return 0;
	return *reinterpret_cast<const uint16_t *>(name - offs::MODELINFO_NAME + offs::MODELINFO_REFCOUNT);
}

void LoadRequestedModelsNow() {
	Func<VoidFn>(CTimer__Stop)();
	Func<LoadAllFn>(CStreaming__LoadAllRequestedModels)(0);
	Func<VoidFn>(CTimer__Update)();
}

bool ResolveCutsceneModels(const char *cutscene, bool loadFirst, CutsceneModels *out) {
	*out = CutsceneModels{};
	std::snprintf(out->cutscene, sizeof out->cutscene, "%s", cutscene);
	if (!ReadCutsceneAnims(out->cutscene, &out->anims))
		return false;
	if (Resolve(out) == 0 || !loadFirst)
		return true;
	// A model a script asked for a moment ago has its name and no clump
	// until it is in.
	LoadRequestedModelsNow();
	Resolve(out);
	return true;
}

void DescribeCutsceneModels(const CutsceneModels &m, char *out, size_t size) {
	size_t n = 0;
	out[0]   = '\0';
	for (size_t i = 0; i < m.anims.count && n + 1 < size; ++i) {
		const char *const name = NameOfModel(m.model[i]);
		const int w = m.model[i] < 0 ? std::snprintf(out + n, size - n, "%s%s=none", i ? " " : "",
		                                             m.anims.names[i])
		                             : std::snprintf(out + n, size - n, "%s%s=%d('%s')", i ? " " : "",
		                                             m.anims.names[i], m.model[i], name ? name : "?");
		if (w < 0)
			break;
		n += static_cast<size_t>(w);
	}
}

void DescribeSpecialModels(char *out, size_t size) {
	size_t n = 0;
	out[0]   = '\0';
	const int32_t ids[] = {MI_PLAYER, MI_SPECIAL01, MI_SPECIAL01 + 1, MI_SPECIAL01 + 2, MI_SPECIAL01 + 3};
	for (int32_t id : ids) {
		const char *const name = NameOfModel(id);
		const int w = std::snprintf(out + n, size - n, "%s%d='%s'%s", n ? " " : "", id, name ? name : "?",
		                            HasModelLoaded(static_cast<uint32_t>(id)) ? "" : "(not loaded)");
		if (w < 0 || static_cast<size_t>(w) >= size - n)
			break;
		n += static_cast<size_t>(w);
	}
}

} // namespace coopiii::game
