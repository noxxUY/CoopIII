// A cutscene head with no animation.
//
// addresses.h ("a cutscene head's animation") has the engine side. The short
// of it: CREATE_CUTSCENE_HEAD puts a CCutsceneHead in the world at once, its
// RpHAnim hierarchy starts with no animation, and only SET_HEAD_ANIM gives it
// one. Every frame in between, CCutsceneHead::ProcessControl hands that
// hierarchy to RpHAnimHierarchyAddAnimTime, which reads the duration of the
// animation it does not have - the fault at 0x005B14F7 reading 0x0000000C -
// and Render would call a null key-frame callback right after.
//
// A mission's script never lets a frame in between: the two are consecutive
// instructions. A participant replaying the owner's mission does. Each
// instruction arrives as its own reliable packet, the socket thread queues
// them as they come, and the game thread runs whatever is queued before
// CGame::Process (Client::PreFrame, NetThread::DrainInbound) - so a
// CREATE_CUTSCENE_HEAD can be run in one frame and its SET_HEAD_ANIM in the
// next, or not at all if the replay drops it. And an .anm missing from this
// install's cuts.img leaves the head without one for the whole scene, in
// single player too.
//
// So the three calls the head makes are taken (cutscenehead.cpp). While its
// hierarchy has no animation, AddAnimTime and UpdateHierarchyMatrices are not
// called and the head is not drawn: it holds still and out of sight until
// SET_HEAD_ANIM gives it one, then plays from the start, as it would have.
// With one, all three are the engine's own calls, unchanged.
//
// The decisions are pure, for tools/clienttest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game {

// The two fields of an RpHAnimHierarchy that RpHAnimHierarchySetCurrentAnim
// fills together and that the head's two calls read: the animation, whose
// duration AddAnimTime reads, and the key-frame-to-matrix callback
// UpdateHierarchyMatrices calls. `hier` is the hierarchy's first bytes, at
// least HEAD_HIER_SPAN of them, or null.
constexpr size_t HEAD_HIER_CURRENT_ANIM = 0x08;
constexpr size_t HEAD_HIER_TO_MATRIX    = 0x40;
constexpr size_t HEAD_HIER_SPAN         = HEAD_HIER_TO_MATRIX + 4;

inline bool HeadHierarchyAnimated(const uint8_t *hier) {
	if (!hier)
		return false;
	uint32_t anim = 0, toMatrix = 0;
	std::memcpy(&anim, hier + HEAD_HIER_CURRENT_ANIM, 4);
	std::memcpy(&toMatrix, hier + HEAD_HIER_TO_MATRIX, 4);
	return anim != 0 && toMatrix != 0;
}

// Where a head the guard stopped stands, for the one line that says so:
// among the objects of the scene that is loaded now, or not - one left over
// from a scene already cleared, or one made with no scene loaded at all.
enum class HeadPlace : uint8_t {
	InLoadedScene,   // one of ms_pCutsceneObjects[0..count), a scene loaded
	NotInScene,      // a scene is loaded and the head is not one of its
	NoScene,         // no scene loaded
};

inline HeadPlace PlaceOfHead(const void *head, const void *const *objects, int32_t count,
                             int32_t max, bool sceneLoaded) {
	if (!sceneLoaded)
		return HeadPlace::NoScene;
	if (count > max)
		count = max;
	for (int32_t i = 0; objects && i < count; ++i)
		if (objects[i] == head)
			return HeadPlace::InLoadedScene;
	return HeadPlace::NotInScene;
}

inline const char *DescribeHeadPlace(HeadPlace p) {
	switch (p) {
	case HeadPlace::InLoadedScene: return "one of the loaded scene's own";
	case HeadPlace::NotInScene:    return "not one of the loaded scene's, so left over from another";
	case HeadPlace::NoScene:       return "with no scene loaded at all, so left over from one";
	}
	return "?";
}

// The three calls, taken together or not at all. False leaves the head the
// engine's, as before.
bool InstallCutsceneHeadGuard();
void RemoveCutsceneHeadGuard();

// How many frames a head has been held because it had no animation, for the
// frame heartbeat.
uint32_t CutsceneHeadsHeld();

} // namespace coopiii::game
