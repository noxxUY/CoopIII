// The session's clock on the engine side of the bridge.
//
// Client::PreFrame estimates the server's clock (sessiontime.h) and hands it
// over here once a frame, before CGame::Process. The detours that run the
// clock-driven parts of the world on it - the trains (trains.cpp), the planes
// (planes.cpp), the traffic lights (lights.cpp) and the lift bridge
// (liftbridge.cpp) - read it back at the moment the engine calls them, so
// they share one estimate rather than keeping one each.
#pragma once

#include "client.h"

#include <cstdint>

namespace coopiii::game {

// The session's time right now, read off the wall clock at the moment of the
// call. False when there is no server clock to follow - not connected, or not
// heard from yet - and the caller should leave the engine on CTimer's own.
bool SessionClockNow(uint32_t &out);

// The same clock, read once a frame: the first call after PreFrame takes
// SessionClockNow and every later call until the next PreFrame gets that
// value again. For readers the engine calls many times a frame and expects to
// agree with each other, the way they would on CTimer, which only moves in
// CTimer::Update. False exactly when SessionClockNow is.
bool SessionClockThisFrame(uint32_t &out);

// Wires WorldBridge::SetSessionClock. Harmless when no detour is installed:
// the value is simply never read.
void AddSessionClockToBridge(WorldBridge &bridge);

} // namespace coopiii::game
