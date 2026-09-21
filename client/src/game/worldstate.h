// Time of day and weather - the engine side of the world half of WorldBridge.
//
// Small enough to read in one sitting, and deliberately in its own file
// rather than folded into ped.cpp: none of it touches an entity, every
// address it uses came out of one pass over two script opcodes, and keeping
// that provenance together is the same reason combat.cpp exists.
//
// What this does not do is decide anything. Whether a clock is far enough
// out to be worth moving, and who gets to say what the time is, both live in
// client.cpp where they can be tested without a game.
#pragma once

#include "client.h"

namespace coopiii::game {

// Adds the four world functions to a bridge that has already been built.
// Called from dllmain so this stays independent of ped.cpp's MakeWorldBridge.
void AddWorldToBridge(WorldBridge &bridge);

// Reads CClock and CWeather. False in the frontend and on a loading screen,
// where the globals still hold whatever the last session left in them.
bool SampleWorld(WorldState &out);

// CClock::SetGameClock. A jump, so the caller is expected to have decided it
// is worth one.
void ApplyWorldTime(uint8_t hour, uint8_t minute);

// Writes the pair CWeather blends between, and pins ForcedWeatherType so the
// local rotation stops choosing its own next type.
void ApplyWorldWeather(uint8_t weather, uint8_t weatherOld);

// Unpins it again.
void ReleaseWorldWeather();

} // namespace coopiii::game
