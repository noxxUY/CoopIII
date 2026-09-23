// The El and the subway, on the session's clock.
//
// Every train's position is worked out from CTimer::m_snTimeInMilliseconds
// in CTrain::UpdateTrains and from nothing else (addresses.h, "trains"), and
// that clock is different on every machine. So this detours UpdateTrains and
// hands it the session's clock instead, for that one call, then puts CTimer's
// back. No train state travels and none needs to.
//
// What decides the session's clock lives in client.cpp and sessiontime.h,
// where it can be tested, and sessionclock.cpp carries it to here. This file
// only applies it.
#pragma once

namespace coopiii::game {

// Detours CTrain::UpdateTrains. False if the detour couldn't be installed,
// in which case every machine runs its own trains, as before.
bool InstallTrainClock();
void RemoveTrainClock();

} // namespace coopiii::game
