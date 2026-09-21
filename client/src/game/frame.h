// The per-frame pump: the one place CoopIII is allowed to touch the world.
//
// docs/protocol.md §1.1: the game is single-threaded and frame-driven, with
// no engine tick separate from the render frame. Everything CoopIII does to
// game state happens inside this hook, on the game thread. The socket
// thread only ever parks messages in a queue (client/src/queue.h).
//
// The hook wraps CGame::Process (Game.cpp:1002), verified, the frame's
// simulation step:
//
//     PreProcess()       <- apply inbound state, before the world updates
//     CGame::Process()   <- the game's own frame
//     PostProcess()      <- send our state, after the world has settled
//
// One hook rather than the two finer-grained sites the analysis proposed -
// those are still unverified (docs/addresses-unverified.md), and this split
// already puts each callback on the correct side of the simulation anyway.
#pragma once

#include <cstdint>

namespace coopiii::game {

using FrameFn = void (*)();

// Installs the detour. Must NOT be called from DllMain - see docs/compat.md
// §2.2 and the wait in dllmain.cpp. Either callback may be null.
bool InstallFrameHook(FrameFn preProcess, FrameFn postProcess);

void RemoveFrameHook();
bool FrameHookInstalled();

// Frames observed since installing. Cheapest possible proof the hook is
// actually live - matters when a mod conflict would otherwise look like a
// network problem.
uint32_t FramesSeen();

// Blocks until the game loop has actually run, so hooks are installed after
// every other plugin has finished patching. Returns false on timeout.
bool WaitForGameLoop(uint32_t timeoutMs);

} // namespace coopiii::game
