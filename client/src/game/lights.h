// The traffic lights, on the session's clock.
//
// CTrafficLights::LightForCars1, LightForCars2 and LightForPeds are the only
// way anything in GTA III asks a light what it shows, and each one is the
// clock, masked, against three thresholds (addresses.h, "traffic lights";
// lighttime.h). So this detours the three and answers from the session's
// clock. No light state travels and none needs to.
#pragma once

namespace coopiii::game {

// Detours the three light functions. False if any of them couldn't be
// installed; the ones that could stay installed, and a light whose function
// isn't hooked runs on this machine's clock, as before.
bool InstallLightClock();
void RemoveLightClock();

} // namespace coopiii::game
