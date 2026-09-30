// A cutscene's animations, and the models they are laid over.
//
// LOAD_CUTSCENE loads <name>.IFP out of anim\cuts.img, and for every
// animation in it the engine builds a clump of the model named like the
// animation, to lay the animation over (addresses.h, "a cutscene's
// animations"). It never checks there is one: a name with no model loaded
// under it is a read of address 0 inside CAnimBlendAssocGroup::CreateAssociations.
//
// The script makes sure there always is one on the machine that runs it. A
// participant runs only what is replayed of it (replay.h), and a model the
// owner's mission renamed some other way is not: Give Me Liberty dresses 8-Ball
// as 'eight2' with UNDRESS_CHAR, and Luigi's scene right after names an
// animation 'eight2'. So the owner sends, ahead of each scene, the models every
// one of its animations is laid over there (CutsceneModelCode), and a
// participant loads the scene only once each of its animations has a model
// (mission.cpp, RunEffect); one that still has none skips the scene rather
// than fault in the middle of loading it. The scene's objects and heads are
// held to the same rule, since the engine reads through a clump it never
// got just as blindly.
//
// Finding the scene and reading its animations' names is pure, for
// tools/clienttest; cutscene.cpp reads the files and asks the engine.
#pragma once

#include "look.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game {

// CAnimBlendHierarchy::m_name is 24 bytes: SetName (0x004019C0), which
// LoadAnimFile calls with each NAME chunk, is `push 18h` and a strncpy.
constexpr size_t ANIM_NAME_LEN      = 24;
constexpr size_t CUTSCENE_ANIMS_MAX = 32;   // the most in any stock scene is ten

struct CutsceneAnims {
	size_t count = 0;
	char   names[CUTSCENE_ANIMS_MAX][ANIM_NAME_LEN] = {};
};

// The instructions that belong to a loaded scene and do nothing but harm
// without one: SET_CUTSCENE_ANIM on a scene that is not loaded dereferences
// the association it did not find.
namespace cutscene_op {
constexpr uint16_t SET_CUTSCENE_OFFSET    = 0x0244;
constexpr uint16_t CREATE_CUTSCENE_OBJECT = 0x02E5;
constexpr uint16_t SET_CUTSCENE_ANIM      = 0x02E6;
constexpr uint16_t START_CUTSCENE         = 0x02E7;
constexpr uint16_t CREATE_CUTSCENE_HEAD   = 0x02F4;
constexpr uint16_t SET_HEAD_ANIM          = 0x02F5;
constexpr uint16_t LOAD_SPECIAL_CHARACTER = 0x023C;
constexpr uint16_t REQUEST_MODEL          = 0x0247;
constexpr uint16_t LOAD_SPECIAL_MODEL     = 0x02F3;
} // namespace cutscene_op

inline bool IsCutsceneStep(uint16_t opcode) {
	using namespace cutscene_op;
	return opcode == SET_CUTSCENE_OFFSET || opcode == CREATE_CUTSCENE_OBJECT ||
	       opcode == SET_CUTSCENE_ANIM || opcode == START_CUTSCENE ||
	       opcode == CREATE_CUTSCENE_HEAD || opcode == SET_HEAD_ANIM;
}

// The scene an encoded LOAD_CUTSCENE names (replay.h): the eight bytes of
// text after the opcode, as the handler strncpy's them.
inline bool CutsceneNameOf(const uint8_t *code, size_t length, char (&out)[9]) {
	if (length < 2 + 8)
		return false;
	std::memcpy(out, code + 2, 8);
	out[8] = '\0';
	return out[0] != '\0';
}

inline char LowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// anim\cuts.dir: 32 bytes an entry, the offset and the size in 2 KB sectors,
// then a 24-byte name. CDirectory::FindItem compares the names without case.
constexpr size_t CUTS_DIR_ENTRY = 32;
constexpr size_t CUTS_SECTOR    = 2048;

inline bool FindInCutsDir(const uint8_t *dir, size_t size, const char *file, uint32_t *offset,
                          uint32_t *sectors) {
	for (size_t at = 0; at + CUTS_DIR_ENTRY <= size; at += CUTS_DIR_ENTRY) {
		const char *name = reinterpret_cast<const char *>(dir + at + 8);
		size_t      i    = 0;
		while (i < 24 && name[i] != '\0' && file[i] != '\0' && LowerAscii(name[i]) == LowerAscii(file[i]))
			++i;
		const bool nameEnds = i == 24 || name[i] == '\0';
		if (!nameEnds || file[i] != '\0')
			continue;
		std::memcpy(offset, dir + at, 4);
		std::memcpy(sectors, dir + at + 4, 4);
		return true;
	}
	return false;
}

// The animations of an .IFP in the order LoadAnimFile numbers them: "ANPK",
// "INFO" with the count first, then a "NAME" and a "DGAN" for each. Every
// chunk is an id and a size, and the loader rounds the size up to four
// (0x00403B5A, `and ebp,3`). False for anything that is not laid out so.
inline bool ReadIfpAnimNames(const uint8_t *ifp, size_t size, CutsceneAnims *out) {
	*out = CutsceneAnims{};
	auto chunk = [&](size_t at, const char *id, uint32_t *len) {
		if (at + 8 > size || std::memcmp(ifp + at, id, 4) != 0)
			return false;
		std::memcpy(len, ifp + at + 4, 4);
		return true;
	};
	auto rounded = [](uint32_t n) { return (static_cast<size_t>(n) + 3) & ~static_cast<size_t>(3); };
	uint32_t len = 0;
	if (!chunk(0, "ANPK", &len) || !chunk(8, "INFO", &len) || len < 4 || 16 + len > size)
		return false;
	uint32_t count = 0;
	std::memcpy(&count, ifp + 16, 4);
	if (count > CUTSCENE_ANIMS_MAX)
		return false;
	size_t at = 16 + rounded(len);
	for (uint32_t i = 0; i < count; ++i) {
		if (!chunk(at, "NAME", &len) || at + 8 + len > size)
			return false;
		const size_t n = len < ANIM_NAME_LEN - 1 ? len : ANIM_NAME_LEN - 1;
		std::memcpy(out->names[i], ifp + at + 8, n);
		out->names[i][n] = '\0';
		at += 8 + rounded(len);
		if (!chunk(at, "DGAN", &len) || at + 8 + len > size)
			return false;
		at += 8 + rounded(len);
	}
	out->count = count;
	return true;
}

constexpr uint16_t CUTSCENE_MI_PLAYER     = 0;
constexpr uint16_t CUTSCENE_MI_SPECIAL01  = 26;   // addresses.h, MI_SPECIAL01
constexpr uint16_t CUTSCENE_SPECIAL_CHARS = 4;
// The scene props, which LOAD_SPECIAL_MODEL names: CreateCutsceneObject
// treats 0xB9..0xBD as them (addresses.h, MI_CUTOBJ01).
constexpr uint16_t CUTSCENE_MI_CUTOBJ01   = 185;
constexpr uint16_t CUTSCENE_CUTOBJS       = 5;
constexpr size_t   CUTSCENE_MODEL_CODE    = 2 + 5 + 8;

// The instruction that asks for `model` under `name`, the way the scripts ask
// for one, for a participant's engine to have what an animation of the scene
// is laid over here: LOAD_SPECIAL_CHARACTER for special01..04,
// LOAD_SPECIAL_MODEL for the five props, REQUEST_MODEL for any other model,
// whose name is its own on every machine and never given another. 0 for
// none: model 0 and a special slot in one of Claude's outfits are the
// player's clothes, which are each machine's own (look.h).
inline size_t CutsceneModelCode(uint16_t model, const char *name, uint8_t (&code)[CUTSCENE_MODEL_CODE]) {
	if (model == CUTSCENE_MI_PLAYER || !name || name[0] == '\0' || LookIsClaude(name))
		return 0;
	size_t len = 0;
	while (len < ANIM_NAME_LEN && name[len] != '\0')
		++len;
	const bool special = model >= CUTSCENE_MI_SPECIAL01 && model < CUTSCENE_MI_SPECIAL01 + CUTSCENE_SPECIAL_CHARS;
	const bool prop    = model >= CUTSCENE_MI_CUTOBJ01 && model < CUTSCENE_MI_CUTOBJ01 + CUTSCENE_CUTOBJS;
	// Eight bytes with no end to them would be read past by the handler's
	// strncpy, so a name has to leave room for its terminator.
	if ((special || prop) && len >= 8)
		return 0;
	uint16_t opcode = cutscene_op::REQUEST_MODEL;
	int32_t  value  = model;
	if (special) {
		opcode = cutscene_op::LOAD_SPECIAL_CHARACTER;
		value  = model - CUTSCENE_MI_SPECIAL01 + 1;
	} else if (prop) {
		opcode = cutscene_op::LOAD_SPECIAL_MODEL;
	}
	std::memset(code, 0, sizeof code);
	code[0] = static_cast<uint8_t>(opcode & 0xFF);
	code[1] = static_cast<uint8_t>(opcode >> 8);
	code[2] = 0x01;   // an int32 literal (scripts::PARAM_INT32)
	std::memcpy(code + 3, &value, 4);
	if (opcode == cutscene_op::REQUEST_MODEL)
		return 7;
	for (size_t i = 0; i < len; ++i)
		code[7 + i] = static_cast<uint8_t>(LowerAscii(name[i]));
	return CUTSCENE_MODEL_CODE;
}

// The model an encoded LOAD_SPECIAL_CHARACTER or LOAD_SPECIAL_MODEL asks for,
// and the name, lowered as both handlers lower it. False for anything else.
inline bool SpecialLoadTarget(const uint8_t *code, size_t length, int32_t *model, char (&name)[9]) {
	if (length < CUTSCENE_MODEL_CODE || code[2] != 0x01)
		return false;
	const uint16_t opcode = static_cast<uint16_t>(code[0] | (code[1] << 8));
	int32_t        value  = 0;
	std::memcpy(&value, code + 3, 4);
	if (opcode == cutscene_op::LOAD_SPECIAL_CHARACTER) {
		if (value < 1 || value > CUTSCENE_SPECIAL_CHARS)
			return false;
		*model = CUTSCENE_MI_SPECIAL01 + value - 1;
	} else if (opcode == cutscene_op::LOAD_SPECIAL_MODEL) {
		*model = value;
	} else {
		return false;
	}
	for (size_t i = 0; i < 8; ++i)
		name[i] = LowerAscii(static_cast<char>(code[7 + i]));
	name[8] = '\0';
	return *model >= 0 && name[0] != '\0';
}

// Whether a model called `now` would be given another name by asking for
// `wanted`: RequestSpecialModel's own test, which is case-sensitive, on names
// both handlers have lowered already.
inline bool IsRename(const char *now, const char *wanted) {
	return !now || std::strncmp(now, wanted, ANIM_NAME_LEN) != 0;
}

// ---- the engine half ---------------------------------------------------------------

// What LOAD_ALL_MODELS_NOW's handler does (addresses.h,
// LOAD_ALL_MODELS_NOW_HANDLER), called without a script around it.
void LoadRequestedModelsNow();

// How many entities are built from model `id` (CBaseModelInfo::m_refCount).
uint16_t ModelRefs(int32_t id);

// What each animation of a scene is laid over here: the model id
// GetModelFromName finds for it, or -1 for none.
struct CutsceneModels {
	char          cutscene[9] = {};
	CutsceneAnims anims;
	int32_t       model[CUTSCENE_ANIMS_MAX] = {};
	size_t        missing = 0;   // how many have none
};

// Reads the scene's animations out of anim\cuts.dir and anim\cuts.img, the
// way LoadCutsceneData finds them, and what each is laid over here. When
// some have no model and `loadFirst`, whatever is asked for and not in yet is
// loaded first, as LOAD_ALL_MODELS_NOW loads it, and they are looked for
// again. False when the scene's animations could not be read.
bool ResolveCutsceneModels(const char *cutscene, bool loadFirst, CutsceneModels *out);

// One line for the log: every animation and what it is laid over.
void DescribeCutsceneModels(const CutsceneModels &m, char *out, size_t size);
// The clothes and special characters this machine has now, for the log.
void DescribeSpecialModels(char *out, size_t size);

// Whether model `id` is one a script asked for (STREAMFLAGS_SCRIPTOWNED),
// and its name.
bool ScriptOwnsModel(int32_t id);
const char *NameOfModel(int32_t id);

} // namespace coopiii::game
