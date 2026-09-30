// A pedestrian speaking, heard on every machine (protocol.h, C_PedSpeech).
//
// A ped talks on the machine whose engine runs it: CPed::Say queues a line
// and CPed::ServiceTalking plays it through DMAudio a frame or so later. An
// observer's replica decides nothing, so it never says anything either, and a
// remote player's copy is the same kind of ped. So the host notices the line
// being played - CoopIII takes ServiceTalking's two calls from
// CPed::ProcessControl and looks at what changed across them - and says which
// ped played which sound. The observer hands it to CPed::Say on its copy,
// which applies the same priorities and waits the host's engine did.
//
// What decides whether a line goes out is here, with no engine in it, so
// tools/clienttest can walk it. game/pedspeech.cpp is the engine half.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// Takes ServiceTalking's two call sites. Not fatal: without it nobody hears
// anybody else's crowd, which is where this project was before. The rest of
// the bridge is game/population.cpp's: with the sites not taken its queue
// simply stays empty.
bool InstallPedSpeech();
void RemovePedSpeech();

// Further than this from every other player and nobody would hear it: the
// engine's own ped speech is inaudible well inside it.
constexpr float PED_SPEECH_RANGE_M = 60.0f;

// A whole crowd arguing is a handful of lines a second; this is a ceiling
// against a feedback nobody has found yet, not the expected rate.
constexpr uint32_t PED_SPEECH_MAX_PER_S = 8;

// Is a line from this ped worth a packet? `nearestD2` is the squared flat
// distance to the nearest other player, negative when nobody else's position
// is known.
inline bool PedSpeechWorthSending(uint16_t sound, float nearestD2) {
	if (!PedSpeechSoundValid(sound))
		return false;
	if (!(nearestD2 >= 0.0f))
		return false;
	return nearestD2 <= PED_SPEECH_RANGE_M * PED_SPEECH_RANGE_M;
}

// PED_SPEECH_MAX_PER_S in any one-second window, counted from the first line
// of the window.
class SpeechBudget {
public:
	bool Take(uint32_t nowMs) {
		if (m_used == 0 || nowMs - m_windowMs >= 1000u) {
			m_windowMs = nowMs;
			m_used     = 0;
		}
		if (m_used >= PED_SPEECH_MAX_PER_S)
			return false;
		++m_used;
		return true;
	}

private:
	uint32_t m_windowMs = 0;
	uint32_t m_used     = 0;
};

// Did ServiceTalking play a line, and which? It stamps m_lastSoundStart with
// the frame's time and copies the sound into m_lastQueuedSound when it plays
// one, and touches neither otherwise (client game/crowdaddr.h has the
// instructions). `startBefore` and `startAfter` are m_lastSoundStart either
// side of the call.
inline bool SpokeAcross(uint32_t startBefore, uint32_t startAfter) {
	return startAfter != startBefore;
}

} // namespace coopiii::game
