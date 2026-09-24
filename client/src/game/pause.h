// Opening the menu must not stop the world.
//
// In single player, ESC stopping time is the whole point. In a session it's
// a desync and an exploit: everyone else keeps playing, so the player who
// paused sees a frozen city and then a jump, and anyone getting shot at can
// stop time just by reaching for the menu. The menu still has to *look*
// paused though - the game's own sounds stop, the HUD behaves as it does on
// the pause screen - because that's what the player expects, and none of
// that is simulation anyway.
//
// So this splits "paused" into the two things GTA III packs into one flag.
// The flag stays false while the engine decides how much time passed and
// whether to update the world, and true while the parts that only present
// the game are looking at it.
//
// Turns out stopping the world was doing a third job too, and missing it
// was obvious the moment the world kept running: it was also what stopped
// the player from *acting*. With time still running, a player who opened
// the menu kept right on sprinting, steered by whatever keys they were
// pressing to navigate it. So the controls get taken away separately and
// explicitly now, through CPad::DisablePlayerControls - see
// LockPlayerControls in pause.cpp.
//
// ---------------------------------------------------------------------------
// The flag, and everything that reads it
// ---------------------------------------------------------------------------
//
// `CTimer::m_UserPause` is set by CMenuManager when the menu opens and cleared
// when it closes. `CTimer::GetIsPaused()` is `m_UserPause || m_CodePause`.
// Four places in the whole engine read either one, and each falls on a
// different side of this:
//
//   CTimer::Update        Timer.cpp:107 - `if (GetIsPaused()) ms_fTimeStep = 0`,
//                         and the clock only advances in the else arm. Runs in
//                         Idle *before* CGame::Process, so a callback cannot
//                         reach it; it gets a detour of its own.
//   CGame::Process        Game.cpp:1022 - `if (!GetIsPaused())` gates the
//                         entire world update. Runs after PreFrame.
//   cAudioManager::Service AudioManager.cpp:115 - copies GetIsUserPaused() and
//                         stops the game's sounds on the rising edge. Called
//                         from Idle *after* CGame::Process, so it sees what
//                         PostFrame leaves behind.
//   CHud::Draw            Hud.cpp:1089 and :1316. Render phase, likewise after.
//
// m_CodePause is never touched. A code pause is the engine halting for its
// own reasons, and those reasons aren't a player pressing a key.
//
// ---------------------------------------------------------------------------
// What this costs
// ---------------------------------------------------------------------------
//
// One frozen frame each time the menu opens. FrontEndMenuManager.Process()
// runs inside CGame::Process, after PreFrame has already cleared the flag
// and before the world gate a few lines later - so the frame the menu
// appears on does see a pause. One frame at 60 Hz isn't visible anyway, and
// moving the clear later would mean hooking a second function for nothing.
#pragma once

namespace coopiii::game {

// Detours CTimer::Update. False if the detour couldn't be installed, in
// which case nothing else here does anything and the game pauses normally.
bool InstallPausePolicy();
void RemovePausePolicy();
bool PausePolicyInstalled();

// Called from the frame pump, on either side of CGame::Process. Safe to call
// when the policy isn't installed.
void ClearPauseForTheWorld();
void RestorePauseForPresentation();

// Keep the controls off the player while the chat line is open (game/chat.h),
// through the same bit the menu uses. Either one holding it is enough, so
// closing the menu mid-sentence does not hand the keys back.
void HoldControlsForChat(bool hold);

} // namespace coopiii::game
