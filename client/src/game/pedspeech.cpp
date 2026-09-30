// The engine half of game/pedspeech.h: ServiceTalking's two calls from
// CPed::ProcessControl, taken so a line a ped plays can be noticed. The call
// sites are redirected rather than the function detoured, the way
// game/crowdrange.cpp takes the traffic generator's; game/crowdaddr.h has the
// instructions.
#include "pedspeech.h"

#include "crowdaddr.h"
#include "crowdrange.h"
#include "population.h"
#include "../log.h"

namespace coopiii::game {

namespace {

using ThisFn = void(__thiscall *)(void *);

bool g_taken[2] = {false, false};

// Stands in for `call CPed::ServiceTalking` with the ped in ecx, as both
// sites have it. The original runs first and whole; a line played across it
// is what m_lastSoundStart changing means.
void __fastcall ServiceTalkingHeard(void *ped, void * /*edx*/) {
	const uint32_t before = ped ? Field<uint32_t>(ped, PED_LAST_SOUND_START) : 0;
	Func<ThisFn>(CPed__ServiceTalking)(ped);
	if (!ped)
		return;
	if (SpokeAcross(before, Field<uint32_t>(ped, PED_LAST_SOUND_START)))
		NotePedSpoke(ped, Field<uint16_t>(ped, PED_LAST_QUEUED_SOUND));
}

} // namespace

bool InstallPedSpeech() {
	size_t taken = 0;
	for (size_t i = 0; i < 2; ++i) {
		if (!g_taken[i])
			g_taken[i] = RedirectCallSite(SERVICE_TALKING_CALLS[i], CPed__ServiceTalking,
			                              reinterpret_cast<uintptr_t>(&ServiceTalkingHeard));
		taken += g_taken[i] ? 1 : 0;
	}
	if (taken == 2)
		Log("speech: took ServiceTalking's two calls, so what our crowd and our player say "
		    "is heard on the other machines");
	else
		Log("speech: FAILED to take %u of ServiceTalking's two calls; what our crowd says "
		    "is heard only here",
		    static_cast<unsigned>(2 - taken));
	return taken == 2;
}

void RemovePedSpeech() {
	for (size_t i = 0; i < 2; ++i)
		if (g_taken[i] &&
		    RedirectCallSite(SERVICE_TALKING_CALLS[i],
		                     reinterpret_cast<uintptr_t>(&ServiceTalkingHeard),
		                     CPed__ServiceTalking))
			g_taken[i] = false;
}

} // namespace coopiii::game
