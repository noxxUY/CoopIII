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
//
// ---------------------------------------------------------------------------
// Losing focus is a different stop, and it stops everything
// ---------------------------------------------------------------------------
//
// WinMain's loop (0x00582A10) runs a frame only while ForegroundApp
// (0x0060F000) is set:
//
//   00582A50  cmp [0060F000h],0 / je 00582F06
//   00582F06  RwCameraBeginUpdate(Scene.camera) ? ForegroundApp = 1,
//             RsEventHandler(rsACTIVATE, 1) : nothing
//   00582F37  call [0061D558h]                   WaitMessage
//
// and the only thing that ever clears it is psCameraBeginUpdate (0x00580C70)
// failing, which is what a lost D3D device does: a fullscreen alt-tab, or a
// minimised window on some drivers. From then on there is no CGame::Process
// at all until the device comes back - no frame pump, no snapshots, and a
// car, the traffic this machine hosts and its police helicopter hanging in
// the air on everybody else's screen. Retail's WM_ACTIVATEAPP (0x00581A82)
// only clears the pads; the windowed-mode plugin's autoPause instead opens
// the pause menu through RequestFrontEndStartUp (0x00488770), which the
// policy above already keeps from stopping the world.
//
// So the WaitMessage call is redirected (its FF 15 operand is pointed at a
// pointer of ours; the instruction itself is untouched) and, in a session,
// the engine's own no-render frame runs in its place: exactly what Idle
// (0x0048E480) does before its `if (arg == nil) return` - CTimer::Update,
// CPointLights::NumLights = 0, CGame::Process - and nothing it does after,
// since there is no device to draw with. DMAudio.Service is left out too:
// rsACTIVATE(0) has just released the digital handle. Then a bounded wait for
// a message instead of an unbounded one. The engine's own retry of
// RwCameraBeginUpdate at 00582F06 still runs every time round and puts the
// normal loop back the moment the device does.
#pragma once

#include <cstdint>

namespace coopiii::game {

// WinMain's `call [WaitMessage]` in the ForegroundApp == 0 arm, and the IAT
// slot it reads. dumpbin /imports puts WaitMessage at 0x0061D558 (USER32's
// table at 0x0061D4E0; PeekMessageA, TranslateMessage and DispatchMessageA at
// 0x0061D540/544/548 are the three the loop head at 0x00582A1D calls). Five
// sites call through the slot; only this one is WinMain's. tools/clienttest's
// patch-site table checks the bytes against the retail image.
constexpr uintptr_t WINMAIN_WAITMESSAGE_CALL  = 0x00582F37;
constexpr uintptr_t IAT_WAITMESSAGE           = 0x0061D558;
constexpr uint8_t   WAITMESSAGE_CALL_BYTES[6] = {0xFF, 0x15, 0x58, 0xD5, 0x61, 0x00};

// CPointLights::NumLights, int16. Idle zeroes it right before CGame::Process
// (0x0048E492 `mov word [0095CC3Eh],0`) and AddLight bounds it
// (0x00510819 `cmp word [0095CC3Eh],20h / jge`).
constexpr uintptr_t CPointLights__NumLights       = 0x0095CC3E;
constexpr uintptr_t IDLE_NUMLIGHTS_RESET          = 0x0048E492;
constexpr uintptr_t ADDLIGHT_NUMLIGHTS_BOUND      = 0x00510819;
constexpr uint8_t   IDLE_NUMLIGHTS_RESET_BYTES[9] = {0x66, 0xC7, 0x05, 0x3E, 0xCC,
                                                     0x95, 0x00, 0x00, 0x00};
constexpr uint8_t   ADDLIGHT_NUMLIGHTS_BOUND_BYTES[8] = {0x66, 0x83, 0x3D, 0x3E,
                                                         0xCC, 0x95, 0x00, 0x20};

// C_PlayerAway's byte (protocol.h, PLAYER_AWAY_*): the window first, since the
// windowed-mode plugin opens the menu *because* the window went away.
constexpr uint8_t LocalAwayReason(bool menuUp, bool windowInFront, uint8_t menu,
                                  uint8_t window) {
	return !windowInFront ? window : menuUp ? menu : 0;
}

// Whether the frame WinMain would skip is run anyway.
constexpr bool RunUnfocusedFrame(bool policyInstalled, bool inAGame, bool inSession) {
	return policyInstalled && inAGame && inSession;
}

// The game's window is the one in front. Game thread only: it compares the
// foreground window's thread with the caller's.
bool GameWindowInFront();

// Told once a frame whether there is a session, so a game that is not in one
// stops on focus loss exactly as retail does.
void SetSessionForUnfocusedFrames(bool inSession);

// Detours CTimer::Update. False if the detour couldn't be installed, in
// which case nothing else here does anything and the game pauses normally.
bool InstallPausePolicy();
void RemovePausePolicy();
bool PausePolicyInstalled();

// Called from the frame pump, on either side of CGame::Process. Safe to call
// when the policy isn't installed.
void ClearPauseForTheWorld();
void RestorePauseForPresentation();

// ---- the menu closed for the session's mission --------------------------------
//
// The menu no longer stops the world, so the session's mission can start, and
// its cutscene play, under a player who has it open: the frontend is drawn
// over everything, and he sees the menu while the scene runs behind it. So
// when the session's mission starts for this machine, or one of its
// cutscenes starts here, the menu is shut the way the engine's own Resume
// shuts it: CMenuManager::RequestFrontEndShutDown, which only raises
// m_bShutDownFrontEndRequested and puts the music back to the game's, and
// the next CMenuManager::Process's SwitchMenuOnAndOff does the rest (the
// textures, the pads, the pause flag). Process returns early while the
// screen is faded (`cmp [ebp+453h],0` then GetScreenFadeStatus at
// 0x00485110), so under the trigger's fade the request waits for the scene's
// fade in, as the engine's own would.
//
// Not the save menu: m_bSaveMenuActive (+0x453) is the safehouse's, whose
// Process runs even faded, and a save is not dropped for anybody's mission.
//
// __thiscall on FrontEndMenuManager, no arguments, plain `ret`: `sub esp,8 /
// mov [esp+4],ecx / mov ecx,95CDBEh (DMAudio) / mov byte [0095CD6Ah],1 /
// push 1 / call 0057CCF0 (ChangeMusicMode)`. Its callers are Process's
// ProcessButtonPresses (0x004873D2, 0x004879DE, 0x00487CAF, 0x00487CBF) and
// the new game's (0x004852B6), each `mov ecx,<menu> / call`.
constexpr uintptr_t CMenuManager__RequestFrontEndShutDown           = 0x00488750;
constexpr uint8_t   REQUEST_FRONTEND_SHUTDOWN_BYTES[19]             = {
    0x83, 0xEC, 0x08, 0x89, 0x4C, 0x24, 0x04, 0xB9, 0xBE, 0xCD,
    0x95, 0x00, 0xC6, 0x05, 0x6A, 0xCD, 0x95, 0x00, 0x01};
constexpr uintptr_t CMenuManager__m_bSaveMenuActive                 = 0x008F59D8 + 0x453;
// CMenuManager::Process's first test, where the save menu's byte is read.
constexpr uintptr_t MENU_PROCESS_SAVE_MENU_TEST                     = 0x00485110;
constexpr uint8_t   MENU_PROCESS_SAVE_MENU_TEST_BYTES[7] = {0x80, 0xBD, 0x53, 0x04, 0x00, 0x00, 0x00};

// Whether to shut the menu for the session's mission.
constexpr bool CloseMenuForMission(bool inAGame, bool menuUp, bool saveMenu) {
	return inAGame && menuUp && !saveMenu;
}

// Shuts the pause menu if it is up, the engine's way (above). True when it
// asked. `why` goes to the log.
bool CloseMenuForTheSession(const char *why);

// Keep the controls off the player while the chat line is open (game/chat.h),
// through the same bit the menu uses. Either one holding it is enough, so
// closing the menu mid-sentence does not hand the keys back.
void HoldControlsForChat(bool hold);

} // namespace coopiii::game
