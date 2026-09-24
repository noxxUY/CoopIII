// Chat, the session's notices, the player list and the version mark, on the
// HUD.
//
// The words live in Client (chatfeed.h); this is the half that touches the
// game: the keys, the controls while a line is being typed, the clipboard, and
// the drawing.
//
// **The keys come off the game's own window.** CoopIII subclasses it the first
// frame it is active on the game thread and reads WM_KEYDOWN there, turning
// each press into a character with ToUnicode, so the keyboard layout is the
// player's and nothing depends on the game's message loop calling
// TranslateMessage. The chat key (T by default) opens a line, Enter sends it,
// Escape drops it; the arrows move the caret, Up and Down go back through what
// was sent, Ctrl+V or Shift+Insert pastes, Shift+Delete empties the line. The
// list key (F9) shows or hides the player list. Nothing here is an engine
// address.
//
// **While a line is open the game is kept off the keyboard** three ways, each
// through something addresses.h already proves: CPad::DisablePlayerControls'
// CoopIII bit (game/pause.h, HoldControlsForChat), which every movement,
// steering and weapon accessor checks; the key messages themselves, which are
// swallowed; and CPad::NewKeyState and TempKeyState, which are cleared every
// frame so the menu and anything else that reads the keys directly sees none
// held. Escape therefore cancels the line without opening the menu.
//
// **It draws from the CHud::Draw detour** game/nametag.cpp already holds (one
// address, one detour), in CFont's FONT_BANK - the face the game's own
// subtitles use - and under the same gates the nametags have: the HUD on, no
// widescreen bars, no menu.
#pragma once

#include "client.h"

namespace coopiii::game {

// Virtual-key codes for the two keys, from CoopIII.ini (config.h).
void SetChatKeys(int chatKey, int listKey);

// "CoopIII 0.0.1" in the bottom-left corner, under the radar. On unless
// CoopIII.ini says otherwise.
void SetVersionMarkShown(bool shown);

// Remembers the client whose feed is drawn. Nothing is hooked here; the
// window is subclassed from TickChat, on the game thread, once it is active.
void InstallChat(const Client &client);
void RemoveChat();

// Once a frame, from the frame pump before CGame::Process.
void TickChat();

// From the CHud::Draw detour.
void DrawChatOverlay(const Client &client);

// Fills the chat half of the bridge.
void AddChatToBridge(WorldBridge &bridge);

} // namespace coopiii::game
