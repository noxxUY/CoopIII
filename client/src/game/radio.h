// The car radio, on the session's station and the session's clock.
//
// radiosync.h is the design and docs/radio.md how GTA III's radio works. This
// is the engine side, and it is three call sites pointed at us, no detour:
//
// - cAudioManager::Service's call to cMusicManager::Service
//   (MUSIC_SERVICE_CALL). Around it: before, a station the session just put
//   on the car the local player sits in is made to play now, and a station
//   that has wandered more than a second from the clock is started again
//   where the clock says; after, a station the music manager moved that car
//   to - the radio key, F9, off - is handed to Client for the session.
// - ChangeRadioChannel's call to GetTrackStartPos
//   (CHANGE_RADIO_CHANNEL_START_POS), which is where every station starts in
//   a car: the position becomes the session clock modulo the stream's length.
// - ServiceTrack's restart of a stream that reached the end of its file
//   (SERVICE_TRACK_RESTART), at the same place instead of at 0.
//
// Both halves ask for a session clock and leave the engine alone without one,
// so single player, the menu's radio, cutscene tracks, mission audio, the
// announcements and the user-track player are what they always were.
#pragma once

#include "client.h"

namespace coopiii::game {

// Points the three calls at us. False if any would not go - it no longer
// calls what it did - and says in the log what that costs; the ones that
// went stay.
bool InstallRadioSync();
void RemoveRadioSync();

// Wires WorldBridge's four radio entries.
void AddRadioToBridge(WorldBridge &bridge);

} // namespace coopiii::game
