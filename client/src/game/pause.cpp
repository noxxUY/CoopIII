#include "pause.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

namespace coopiii::game {

namespace {

Detour g_timer;

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

} // namespace

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
	return true;
}

void RemovePausePolicy() {
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
