// A new game straight past the menu, for the lobby's host who started
// everybody's game into one (docs/protocol.md 1.31, the launcher's
// ENV_NEW_GAME).
#pragma once

namespace coopiii::game {

// Hooks the frontend's own frame, so that once the menu has been up a moment
// it starts a new game the way its New Game item does. The one hook CoopIII
// puts in before the game runs, and only for this; it needs HookInit. False
// when it could not go in, and the player picks New Game themselves.
bool ArmNewGameFromMenu();

// Whether it has started one yet.
bool StartedNewGameFromMenu();

} // namespace coopiii::game
