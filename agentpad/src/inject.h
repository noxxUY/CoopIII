// The detour on CPad::UpdatePads, and the writes into the game's input state.
#pragma once

#include <cstdint>

namespace agentpad {

// Hooks CPad::UpdatePads and starts serving whatever `channel` publishes.
// Safe to call once, after the image has been verified and HookInit() has run.
bool InstallPadHook();
void RemovePadHook();
bool PadHookInstalled();

// UpdatePads calls seen since install. Mirrored into the shared section too,
// so a driver can confirm the mod is alive without touching the log.
uint32_t PadFramesSeen();

} // namespace agentpad
