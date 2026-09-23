// The Shoreside lift bridge, on the session's clock.
//
// CBridge::Update raises and lowers the bridge from `(t - epoch) & 0xFFFF`
// (addresses.h, "lift bridge"; liftbridgetime.h). This detours it and, in a
// session, writes the epoch that makes that phase the session clock's own,
// then puts the bridge's traffic links right if the jump onto that clock
// skipped the edge that would have set them. CTimer is never touched.
//
// Only reachable once a save has passed A Drop in the Ocean, the mission
// that runs COMMERCIAL_PASSED. Before that the engine holds the bridge up
// and locked on every machine, and this changes nothing.
#pragma once

namespace coopiii::game {

// Detours CBridge::Update. False if the detour couldn't be installed, in
// which case every machine runs its own bridge, as before.
bool InstallLiftBridgeClock();
void RemoveLiftBridgeClock();

} // namespace coopiii::game
