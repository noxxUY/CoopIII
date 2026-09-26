#include "newgame.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <atomic>
#include <cstdint>

namespace coopiii::game {

namespace {

// How long the menu is up before the new game starts, in frontend frames: long
// enough for it to have loaded what it draws, which the start unloads, and
// for the language and controller pages of a first run to have come up.
constexpr uint32_t MENU_FRAMES_FIRST = 30;

Detour            g_idle;
std::atomic<bool> g_started{false};
uint32_t          g_menuFrames = 0;   // the game thread's alone

using FrontendIdleFn = void(__cdecl *)();
using MenuFn         = void(__thiscall *)(void *);

// FrontendIdle is the frontend's frame: CTimer::Update, the pads, the menu's
// Process, then its drawing. The start goes after it, between two frames of
// the menu, which is where the menu's own New Game leaves things too, and the
// GS_FRONTEND case that called this sees the menu shut as it returns.
void __cdecl HookedFrontendIdle() {
	g_idle.Original<FrontendIdleFn>()();
	if (g_started.load(std::memory_order_relaxed))
		return;
	if (Global<int32_t>(gGameState) != GS_FRONTEND ||
	    Global<uint8_t>(CMenuManager__m_bMenuActive) == 0) {
		g_menuFrames = 0;
		return;
	}
	if (++g_menuFrames < MENU_FRAMES_FIRST)
		return;
	g_started.store(true, std::memory_order_relaxed);
	Log("newgame: starting a new game from the menu, as its New Game does");
	Func<MenuFn>(CMenuManager__DoSettingsBeforeStartingAGame)(Ptr<void>(FrontEndMenuManager));
}

} // namespace

bool ArmNewGameFromMenu() {
	if (g_idle.IsInstalled())
		return true;
	return g_idle.Install("FrontendIdle", reinterpret_cast<void *>(FrontendIdle),
	                      reinterpret_cast<void *>(&HookedFrontendIdle));
}

bool StartedNewGameFromMenu() {
	return g_started.load(std::memory_order_relaxed);
}

} // namespace coopiii::game
