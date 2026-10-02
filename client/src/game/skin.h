// Custom skins, engine side (skinsync.h is the session side; addresses.h,
// "the player's skin", is how the engine draws one).
//
// Each remote player's ped is drawn in a texture of its own: theirs once it
// has come in, the game's default skin before that and whenever the session
// says custom skins are off. Never ours. The local player's skin is left
// entirely to the engine.
//
// Every frame each remote player's ped has the render callback of its atomics
// swapped for SkinRenderCB, which hands RenderPlayerCB that player's texture
// in place of ours for the one call. The textures are made here, from pixels
// in memory, and held one per player; nothing is written to the skins folder.
// Dropping one never frees it under a ped that still shows it: the material
// it is on holds a reference of its own.
#pragma once

#include "client.h"

namespace coopiii::game {

// SkinBridge's four entries.
void AddSkinsToBridge(WorldBridge &bridge);

} // namespace coopiii::game
