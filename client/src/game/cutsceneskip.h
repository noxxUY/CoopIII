// Skipping a cutscene together, on this machine.
//
// protocol.h (CutsceneKey) has the exchange, server/core/cutscenevote.h the
// count and cutsceneskipview.h every decision made here. What is left for
// this file is the engine:
//
// 1. **The skip input.** CCutsceneMgr::Update decides a scene can be skipped
//    and that a skip input went down, and then calls FinishCutscene from one
//    place (addresses.h, "skipping a cutscene"). That call is redirected here:
//    the game has already said yes to the skip when it arrives, so everything
//    it looks at - the camera, the load, the credits, which five inputs - stays
//    the game's. With somebody else in the scene it becomes a vote instead; a
//    participant of the session's mission alone in the count does nothing with
//    it, since the scene is its owner's; anybody else alone skips, the game's
//    own way.
//
// 2. **The intro.** 00_intro.sc also skips itself, on its own IS_BUTTON_PRESSED
//    of Cross or Start. While somebody else is in the intro with us that
//    question is answered "not pressed", at the one call where the handler
//    hands its answer on; and once the count says skip, "pressed" for a few
//    frames, so the script skips the rest of the intro as it would for a
//    player.
//
// 3. **What the game is in.** Once a frame, the engine's own skip test
//    without the buttons, told to the server whenever it changes, and a skip
//    the server ordered carried out through FinishCutscene as Update would.
//
// 4. **The count.** "Skip 1/2 - press Enter" in the bottom-right corner,
//    beside nothing, in the version mark's face (chat.h, DrawCornerMark); only
//    while the scene can be skipped and somebody else is in it.
#pragma once

namespace coopiii {
class Client;
}

namespace coopiii::game {

// The two call sites. False when the skip input's could not be taken, which
// leaves every skip local, as before, and the participant's buttons held
// (mission.cpp, HoldSkipButtons).
bool InstallCutsceneSkip(Client &client);
void RemoveCutsceneSkip();

// Is the skip input's call ours? mission.cpp holds a participant's skip
// buttons only when it is not.
bool CutsceneSkipRedirected();

// Once a frame, from the frame pump before CGame::Process.
void TickCutsceneSkip();

// From the CHud::Draw detour.
void DrawCutsceneSkip();

} // namespace coopiii::game
