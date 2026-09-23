// The background planes, on the session's clock.
//
// CPlane::UpdatePlanes works out where every airliner and every Dodo is from
// CTimer::m_snTimeInMilliseconds (addresses.h, "planes"), exactly as
// UpdateTrains does for the trains, so this is trains.cpp over again: detour
// the one call, hand it the session's clock, put CTimer's back. The only
// addition is the two mission Cessnas, whose start times are moved with the
// clock for the length of the call (planetime.h says why).
#pragma once

namespace coopiii::game {

// Detours CPlane::UpdatePlanes. False if the detour couldn't be installed,
// in which case every machine flies its own planes, as before.
bool InstallPlaneClock();
void RemovePlaneClock();

} // namespace coopiii::game
