#include "pause.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

Detour g_timer;

// ---- the frame a lost focus skips (pause.h) ----------------------------------

// The wait between two unfocused frames: about the rate the game runs at.
constexpr DWORD UNFOCUSED_FRAME_MS = 16;

using WaitFn    = BOOL(WINAPI *)();
using ProcessFn = void(__cdecl *)();

bool       g_inSession    = false;
bool       g_waitTaken    = false;
bool       g_saidUnfocus  = false;
uint32_t   g_unfocusedRun = 0;
// What the call now reads through: a pointer to our function.
WaitFn     g_waitVia      = nullptr;

BOOL WINAPI WaitOrRunUnfocused();

// The chat line is open (HoldControlsForChat).
bool g_chatHolds = false;

// CTimer::Update is a static member (re3 Timer.h), so __cdecl with no
// arguments - same reasoning as CGame::Process in frame.cpp. Get this wrong
// and the stack corrupts sixty times a second.
using UpdateFn = void(__cdecl *)();

bool &UserPause() { return Global<bool>(CTimer__m_UserPause); }

bool MenuIsUp() { return Global<bool>(CMenuManager__m_bMenuActive); }

// Only while a game is actually being played.
//
// CTimer::Update also runs on the title screen, through FrontendIdle, and
// there's nothing there that needs keeping unpaused - but there is
// something that can break: the attract loop is driven off
// CTimer::GetTimeInMilliseconds, and letting that clock run during the menu
// would start counting down to a demo the player never asked for.
// gGameState is the one readiness signal that doesn't go through CTimer at
// all, which is exactly why it's the right one to gate on here (addresses.h,
// the startup state machine).
bool InAGame() { return Global<int>(gGameState) == GS_PLAYING_GAME; }

// Take the controls off the player, the engine's own way.
//
// The first version of this didn't, and it showed immediately: with the
// world no longer stopped, a player who opened the menu just kept running,
// steered by whatever keys they were pressing to navigate it. Stopping the
// world turned out to be doing two jobs, and time was only one of them.
//
// CPad::DisablePlayerControls is a bitmask every movement, steering and
// weapon accessor in CPad consults before returning anything, so setting it
// silences the player without touching the menu at all - the menu reads
// NewState directly. We own exactly one bit and never disturb the others,
// which matters: the game sets its own for cutscenes, garages, the phone
// and MakePlayerSafe, and clearing one of those on the way out of a menu
// would hand the player control back in the middle of a cutscene.
// addresses.h has the proof that our bit is one the retail image never
// writes to.
void LockPlayerControls(bool locked) {
	uint8_t &mask = *reinterpret_cast<uint8_t *>(
	    CPad__Pads + pad::DISABLE_PLAYER_CONTROLS);   // GetPad(0) is &Pads[0]
	mask = static_cast<uint8_t>(locked ? (mask | pad::PLAYERCONTROL_COOPIII)
	                                   : (mask & ~pad::PLAYERCONTROL_COOPIII));
}

void __cdecl HookedTimerUpdate() {
	// Bracketed rather than just cleared, because this is the *only* place
	// the flag has to be false that the frame pump can't reach. What happens
	// after this returns is CGame::Process, and PreFrame clears it there on
	// its own terms.
	const bool was = InAGame() && UserPause();
	if (was)
		UserPause() = false;

	g_timer.Original<UpdateFn>()();

	if (was)
		UserPause() = true;
}

// WinMain's WaitMessage, in the frame ForegroundApp says to skip. Called
// through the IAT slot, not the address it held when installed, so a plugin
// that hooks WaitMessage by its import keeps its hook.
BOOL WINAPI WaitOrRunUnfocused() {
	const WaitFn real = *reinterpret_cast<WaitFn *>(IAT_WAITMESSAGE);
	if (!RunUnfocusedFrame(g_timer.IsInstalled(), InAGame(), g_inSession))
		return real();

	if (!g_saidUnfocus) {
		g_saidUnfocus = true;
		Log("pause: the game lost its display (focus gone, device lost); running the "
		    "world without drawing until it comes back, so our car and our traffic keep "
		    "moving on everybody else's screen");
	}
	++g_unfocusedRun;

	// Idle's own order, up to the point where it would start drawing.
	Func<UpdateFn>(CTimer__Update)();
	Global<int16_t>(CPointLights__NumLights) = 0;
	Func<ProcessFn>(CGame__Process)();

	// Bounded: back round the loop for its PeekMessage and its own retry of
	// RwCameraBeginUpdate at least once a frame.
	MsgWaitForMultipleObjects(0, nullptr, FALSE, UNFOCUSED_FRAME_MS, QS_ALLINPUT);
	return TRUE;
}

bool PointWaitAt(const void *target) {
	uint8_t *const site = reinterpret_cast<uint8_t *>(WINMAIN_WAITMESSAGE_CALL);
	DWORD          old  = 0;
	if (!VirtualProtect(site, sizeof WAITMESSAGE_CALL_BYTES, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const uint32_t operand = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
	std::memcpy(site + 2, &operand, sizeof operand);
	VirtualProtect(site, sizeof WAITMESSAGE_CALL_BYTES, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, sizeof WAITMESSAGE_CALL_BYTES);
	return true;
}

// Game thread only (dllmain.cpp installs from the first PreFrame), so WinMain
// is never between fetching this instruction and executing it.
void TakeUnfocusedWait() {
	if (g_waitTaken)
		return;
	if (std::memcmp(reinterpret_cast<const void *>(WINMAIN_WAITMESSAGE_CALL),
	                WAITMESSAGE_CALL_BYTES, sizeof WAITMESSAGE_CALL_BYTES) != 0) {
		Log("pause: WinMain's WaitMessage call at 0x%08X is not the retail `call "
		    "[0061D558h]`; losing focus will stop the game as it does in single player",
		    WINMAIN_WAITMESSAGE_CALL);
		return;
	}
	g_waitVia = &WaitOrRunUnfocused;
	if (!PointWaitAt(&g_waitVia)) {
		Log("pause: could not unprotect 0x%08X; losing focus will stop the game",
		    WINMAIN_WAITMESSAGE_CALL);
		return;
	}
	g_waitTaken = true;
	Log("pause: took WinMain's WaitMessage at 0x%08X; a game that loses its display in a "
	    "session keeps running",
	    WINMAIN_WAITMESSAGE_CALL);
}

void GiveBackUnfocusedWait() {
	if (!g_waitTaken)
		return;
	if (PointWaitAt(reinterpret_cast<const void *>(IAT_WAITMESSAGE)))
		g_waitTaken = false;
	if (g_unfocusedRun != 0)
		Log("pause: ran %u frame(s) without a display", g_unfocusedRun);
}

} // namespace

bool GameWindowInFront() {
	HWND front = GetForegroundWindow();
	return front != nullptr && GetWindowThreadProcessId(front, nullptr) == GetCurrentThreadId() &&
	       !IsIconic(front);
}

void SetSessionForUnfocusedFrames(bool inSession) {
	g_inSession = inSession;
}

bool InstallPausePolicy() {
	if (!g_timer.Install("CTimer::Update", reinterpret_cast<void *>(CTimer__Update),
	                     reinterpret_cast<void *>(&HookedTimerUpdate))) {
		Log("pause: FAILED to hook CTimer::Update at 0x%08X; the menu will stop "
		    "the world as it does in single player",
		    CTimer__Update);
		for (const auto &f : HookFailures())
			Log("pause:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("pause: hooked CTimer::Update at 0x%08X; the menu no longer stops the "
	    "world",
	    CTimer__Update);
	TakeUnfocusedWait();
	return true;
}

void RemovePausePolicy() {
	GiveBackUnfocusedWait();
	if (!g_timer.IsInstalled())
		return;
	g_timer.Remove();
	// Leave the flag agreeing with the menu, so whatever runs next sees a
	// consistent game instead of one paused with no menu up - and give the
	// controls back unconditionally. A player left permanently disabled by a
	// mod that's no longer even running has no way to recover from that.
	UserPause() = MenuIsUp();
	LockPlayerControls(false);
	Log("pause: hook removed");
}

bool PausePolicyInstalled() {
	return g_timer.IsInstalled();
}

void ClearPauseForTheWorld() {
	if (!g_timer.IsInstalled() || !InAGame())
		return;
	UserPause() = false;
}

void RestorePauseForPresentation() {
	if (!g_timer.IsInstalled() || !InAGame())
		return;
	// Read from the menu, not from a value saved in ClearPauseForTheWorld -
	// the menu could've opened or closed in between, and the menu is the
	// thing that was true all along. CMenuManager sets m_bMenuActive and
	// m_UserPause together, including on the save-menu path.
	const bool menu = MenuIsUp();
	UserPause()     = menu;

	// Set here instead of in PreFrame so it's decided *after* the menu has
	// had its say this frame, then read by the next frame's world update.
	// Costs the same single frame the pause itself costs: the frame the menu
	// opens on still moves the player, and the frame it closes on still
	// ignores them.
	LockPlayerControls(menu || g_chatHolds);
}

bool CloseMenuForTheSession(const char *why) {
	if (!CloseMenuForMission(InAGame(), MenuIsUp(), Global<bool>(CMenuManager__m_bSaveMenuActive)))
		return false;
	using ShutDownFn = void(__thiscall *)(void *);
	Func<ShutDownFn>(CMenuManager__RequestFrontEndShutDown)(
	    reinterpret_cast<void *>(FrontEndMenuManager));
	Log("pause: shut the menu for %s", why ? why : "the session's mission");
	return true;
}

void HoldControlsForChat(bool hold) {
	if (hold == g_chatHolds)
		return;
	g_chatHolds = hold;
	// Straight away, and whether or not the pause policy is installed: with
	// the menu pausing the game as in single player, nothing else ever sets
	// or clears our bit.
	LockPlayerControls(hold || (g_timer.IsInstalled() && InAGame() && MenuIsUp()));
}

} // namespace coopiii::game
