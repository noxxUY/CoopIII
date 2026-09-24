// Which body a remote Claude is built from.
//
// Claude's clothes are the name model 0 is loaded under (addresses.h,
// MI_PLAYER): "player", or "playerp" in the opening's prison outfit. Every
// ped built from model 0 on this machine wears whatever *our* model 0 is,
// so a remote player in the other outfit can't use it. They get one of the
// four special-character slots instead, loaded with their look the way
// LOAD_SPECIAL_CHARACTER loads a mission character.
//
// Those slots belong to the missions first. A slot is only taken when
// nothing is built from it and no script or save is holding it, and ped.cpp
// hands it back the moment a script asks for it. With all four in use the
// remote player is built from model 0 in our clothes, which is how it was
// before any of this.
//
// Pure decisions over a snapshot of the slots, so clienttest covers them
// without the game. ped.cpp reads the engine and acts on the answer.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

constexpr int LOOK_SLOTS = 4;   // special01..04, MI_SPECIAL01 onward

// CStreamingInfo::m_flags that mean somebody is holding a model:
// STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_SCRIPTOWNED (addresses.h).
constexpr uint8_t LOOK_HELD_FLAGS = 0x03;

// One special slot, as the streamer and the model info have it right now.
struct LookSlot {
	char     name[PLAYER_LOOK_LEN] = {};   // CBaseModelInfo::m_name
	uint8_t  loadState = 0;                // CStreamingInfo::m_loadState
	uint8_t  flags     = 0;                // CStreamingInfo::m_flags
	uint16_t refs      = 0;                // CBaseModelInfo::m_refCount
	bool     ours      = false;            // we loaded it, nobody has asked since
};

enum class LookPick : uint8_t {
	Model0,    // their look is what our model 0 is, or they never said
	Slot,      // build from special slot `slot`, loading it first if need be
	NoSlot,    // all four are a mission's: model 0, in our clothes
	Refused,   // not something Claude wears: model 0, in our clothes
};

struct LookChoice {
	LookPick pick = LookPick::Model0;
	int      slot = -1;
};

// Model names as the engine keeps them are lower case (UNDRESS_CHAR and
// LOAD_SPECIAL_CHARACTER both lower the script's label), but a save or a
// mod could hand model 0 anything, so this doesn't rely on it.
inline bool SameLook(const char *a, const char *b) {
	if (!a || !b)
		return false;
	for (size_t i = 0; i < PLAYER_LOOK_LEN; ++i) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z')
			x = static_cast<char>(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = static_cast<char>(y - 'A' + 'a');
		if (x != y)
			return false;
		if (x == '\0')
			return true;
	}
	return true;
}

// A special slot is a CPedModelInfo and gets a ped's skeleton built from
// whatever dff it is handed. The only dffs the script ever dresses Claude
// in are player*, so nothing else off the wire goes into one.
inline bool LookIsClaude(const char *look) {
	static constexpr char prefix[] = "player";
	if (!look)
		return false;
	for (size_t i = 0; i + 1 < sizeof prefix; ++i)
		if (look[i] != prefix[i])
			return false;
	return true;
}

// Nothing built from it, nobody holding it, and not half way through a load
// somebody else asked for.
inline bool LookSlotFree(const LookSlot &s) {
	return !s.ours && s.refs == 0 && (s.flags & LOOK_HELD_FLAGS) == 0 &&
	       (s.loadState == 0 || s.loadState == 1);
}

// `look` is the remote player's, `model0` what our model 0 is called.
//
// A slot we already loaded with this look comes first, so two players in the
// same clothes share one. Then a free slot that happens to hold the look
// already, then any free slot from the top down: missions and cutscenes fill
// special01 first, so special04 is the one least likely to be asked for.
inline LookChoice PickLookSlot(const char *look, const char *model0,
                               const LookSlot (&slots)[LOOK_SLOTS]) {
	if (!look || look[0] == '\0' || SameLook(look, model0))
		return {LookPick::Model0, -1};
	if (!LookIsClaude(look))
		return {LookPick::Refused, -1};

	for (int i = 0; i < LOOK_SLOTS; ++i)
		if (slots[i].ours && SameLook(slots[i].name, look))
			return {LookPick::Slot, i};
	for (int i = LOOK_SLOTS - 1; i >= 0; --i)
		if (LookSlotFree(slots[i]) && SameLook(slots[i].name, look))
			return {LookPick::Slot, i};
	for (int i = LOOK_SLOTS - 1; i >= 0; --i)
		if (LookSlotFree(slots[i]))
			return {LookPick::Slot, i};
	return {LookPick::NoSlot, -1};
}

// Whether a slot we hold can be let go: nothing is built from it and no
// remote player is waiting on it.
inline bool LookSlotReleasable(const LookSlot &s, bool wanted) {
	return s.ours && !wanted && s.refs == 0;
}

} // namespace coopiii::game
