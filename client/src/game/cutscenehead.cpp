#include "cutscenehead.h"

#include "addresses.h"
#include "leadcheck.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

static_assert(HEAD_HIER_CURRENT_ANIM == offs::HANIM_CURRENT_ANIM &&
                  HEAD_HIER_TO_MATRIX == offs::HANIM_KEYFRAME_TO_MATRIX &&
                  HEAD_HIER_SPAN == offs::HANIM_READ_SPAN,
              "cutscenehead.h reads the hierarchy where addresses.h says it is");

namespace {

using AddAnimTimeFn    = int(__cdecl *)(void *, float);
using UpdateMatricesFn = int(__cdecl *)(void *);
using ObjectRenderFn   = void(__thiscall *)(void *);
using FirstAtomicFn    = void *(__cdecl *)(void *);
using AtomicHierFn     = void *(__cdecl *)(void *);

bool     g_installed = false;
bool     g_said      = false;
uint32_t g_held      = 0;

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

bool Animated(void *hier) { return HeadHierarchyAnimated(static_cast<const uint8_t *>(hier)); }

// The head's hierarchy, found the way its ProcessControl and Render find it.
void *HierarchyOf(void *head) {
	void *const clump = Field<void *>(head, offs::RW_OBJECT);
	if (!clump)
		return nullptr;
	void *const atomic = Func<FirstAtomicFn>(GetFirstAtomic)(clump);
	return atomic ? Func<AtomicHierFn>(RpSkinAtomicGetHAnimHierarchy)(atomic) : nullptr;
}

void SayHeld(void *hier, void *head, const char *where) {
	++g_held;
	if (g_said)
		return;
	g_said = true;
	const int32_t count = Global<int32_t>(CCutsceneMgr__ms_numCutsceneObjs);
	const bool    loaded = Global<uint8_t>(CCutsceneMgr__ms_running) != 0;
	char          scene[9];
	std::memcpy(scene, Ptr<const char>(CCutsceneMgr__ms_cutsceneName), 8);
	scene[8] = '\0';
	const HeadPlace place =
	    head ? PlaceOfHead(head, Ptr<const void *const>(CCutsceneMgr__ms_pCutsceneObjects), count,
	                       CUTSCENE_OBJECTS_MAX, loaded)
	         : HeadPlace::NoScene;
	Log("cutscene: a cutscene head%s had no animation when the engine %s it (hierarchy %08X), "
	    "and would have read through it; it holds still and undrawn until SET_HEAD_ANIM gives it "
	    "one. It is %s: scene '%s' %s, %d object(s) (said once)",
	    head ? "" : " (not found from its hierarchy)", where,
	    static_cast<unsigned>(reinterpret_cast<uintptr_t>(hier)),
	    head ? DescribeHeadPlace(place) : "?", scene, loaded ? "loaded" : "not loaded",
	    static_cast<int>(count));
	if (head)
		Log("cutscene: that head is %08X, model %d", static_cast<unsigned>(reinterpret_cast<uintptr_t>(head)),
		    static_cast<int>(Field<int16_t>(head, offs::MODEL_INDEX)));
}

// The scene's head whose hierarchy this is, for the line: AddAnimTime and
// UpdateHierarchyMatrices are handed the hierarchy and not the head.
void *HeadOf(void *hier) {
	const int32_t count = Global<int32_t>(CCutsceneMgr__ms_numCutsceneObjs);
	void *const  *objects = Ptr<void *const>(CCutsceneMgr__ms_pCutsceneObjects);
	for (int32_t i = 0; i < count && i < CUTSCENE_OBJECTS_MAX; ++i) {
		void *const o = objects[i];
		if (o && Field<uintptr_t>(o, offs::VTABLE) == CCutsceneHead__vtable && HierarchyOf(o) == hier)
			return o;
	}
	return nullptr;
}

// CCutsceneHead::ProcessControl's RpHAnimHierarchyAddAnimTime(hier, step/50).
int __cdecl GuardAddAnimTime(void *hier, float delta) {
	if (Animated(hier))
		return Func<AddAnimTimeFn>(RpHAnimHierarchyAddAnimTime)(hier, delta);
	SayHeld(hier, hier ? HeadOf(hier) : nullptr, "moved");
	return 0;
}

// CCutsceneHead::Render's RpHAnimUpdateHierarchyMatrices(hier).
int __cdecl GuardUpdateMatrices(void *hier) {
	if (Animated(hier))
		return Func<UpdateMatricesFn>(RpHAnimUpdateHierarchyMatrices)(hier);
	SayHeld(hier, hier ? HeadOf(hier) : nullptr, "drew");
	return 0;
}

// CCutsceneHead::Render's CObject::Render, `mov ecx,ebx / call`: __thiscall
// with nothing on the stack, which __fastcall with the spare edx is. A head
// whose skin matrices were never worked out is not drawn with whatever the
// allocation happened to hold.
void __fastcall GuardObjectRender(void *head, void * /*edx*/) {
	if (!head || Field<void *>(head, offs::RW_OBJECT) == nullptr || Animated(HierarchyOf(head))) {
		Func<ObjectRenderFn>(object::CObject__Render)(head);
		return;
	}
}

struct Site {
	uintptr_t   at;
	uintptr_t   engine;
	uintptr_t   ours;
	const char *what;
};

Site Sites[3] = {
    {HEAD_ADD_ANIM_TIME_CALL, RpHAnimHierarchyAddAnimTime, 0, "ProcessControl's AddAnimTime"},
    {HEAD_UPDATE_MATRICES_CALL, RpHAnimUpdateHierarchyMatrices, 0, "Render's UpdateHierarchyMatrices"},
    {HEAD_OBJECT_RENDER_CALL, object::CObject__Render, 0, "Render's CObject::Render"},
};

void FillOurs() {
	Sites[0].ours = reinterpret_cast<uintptr_t>(&GuardAddAnimTime);
	Sites[1].ours = reinterpret_cast<uintptr_t>(&GuardUpdateMatrices);
	Sites[2].ours = reinterpret_cast<uintptr_t>(&GuardObjectRender);
}

} // namespace

bool InstallCutsceneHeadGuard() {
	if (g_installed)
		return true;
	FillOurs();
	// All three still the engine's, or none is taken: a head that is moved
	// but drawn, or drawn but not moved, is the crash one call later.
	for (const Site &s : Sites) {
		if (!RelCallAt(Ptr<uint8_t>(s.at), s.at, s.engine)) {
			Log("cutscene: NOT guarding cutscene heads - %s at 0x%08X is not `call 0x%08X` any "
			    "more (another mod?); a head with no animation reads through it as in single player",
			    s.what, static_cast<unsigned>(s.at), static_cast<unsigned>(s.engine));
			return false;
		}
	}
	size_t done = 0;
	for (; done < 3; ++done)
		if (!RedirectCall(Sites[done].at, Sites[done].engine, Sites[done].ours))
			break;
	if (done != 3) {
		while (done-- > 0)
			RedirectCall(Sites[done].at, Sites[done].ours, Sites[done].engine);
		Log("cutscene: FAILED to take the cutscene head's calls; left as they were");
		return false;
	}
	g_installed = true;
	Log("cutscene: a cutscene head's three calls at 0x%08X, 0x%08X and 0x%08X come to us; one with "
	    "no animation yet holds still instead of reading through it",
	    static_cast<unsigned>(HEAD_ADD_ANIM_TIME_CALL), static_cast<unsigned>(HEAD_UPDATE_MATRICES_CALL),
	    static_cast<unsigned>(HEAD_OBJECT_RENDER_CALL));
	return true;
}

void RemoveCutsceneHeadGuard() {
	if (!g_installed)
		return;
	for (const Site &s : Sites)
		RedirectCall(s.at, s.ours, s.engine);
	g_installed = false;
}

uint32_t CutsceneHeadsHeld() { return g_held; }

} // namespace coopiii::game
