// The scoreboard on the HUD: a panel with everyone in the session, up while
// the scoreboard key (Tab) is held, or pinned with the list key (F9).
// boardlayout.h one level up has the layout and every number; this is the half
// that reads the game and draws.
//
// **It draws after the original CHud::Draw**, from the same detour the tags
// and the chat use. Its panel is CSprite2d::DrawRect, which is drawn at once,
// while text only queues and is drawn at CFont::DrawFonts after the whole HUD.
// Drawn before the HUD, the radar and the weapon icon would land on top of
// the panel. So it flushes the text queued so far first, puts the panel down
// over it and queues its own, which the game's own DrawFonts then draws.
//
// **The key is read, never taken.** GTA III binds nothing to Tab by default:
// CControllerConfigManager::InitDefaultControlConfiguration (0x0058B930 to
// 0x0058BCF3, a run of `push kind / push key / push action / call 0058F700`
// in re3's order) never pushes rsTAB (0x413), and no instruction in the image
// pushes it at all. The press still goes on to the game, in case somebody
// bound it themselves.
#pragma once

#include "client.h"

#include <cstdint>
#include <string>

namespace coopiii::game {

// From CoopIII.ini (config.h): the key held to show it, and the server it
// names in its second line.
void SetScoreboardKey(int vk);
void SetScoreboardServer(const std::string &host, uint16_t port);

// The list key: keeps it up, or lets it go again.
void ToggleScoreboardPin();

// From the CHud::Draw detour, after the original.
void DrawScoreboard(const Client &client);

} // namespace coopiii::game
