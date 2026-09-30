// The car radio in a session: which station a car is on, and where in it.
//
// GTA III plays each radio station as one long stream file that the music
// manager starts wherever it last was, plus the time since (docs/radio.md).
// Two things decide what a player in a car hears, and both were per machine:
//
// - The station. CVehicle::m_nRadioStation, which every copy of a car rolls
//   for itself. The session keeps one per car (protocol.h, S_VehicleRadio)
//   and every copy is made to carry it; what the rules below decide is which
//   difference between the two is the local player turning the dial and
//   which is a copy that has to be put back.
//
// - The position. cMusicManager seeds every stream's position from the local
//   date and a random table at start-up and advances it only while nothing
//   plays it. In a session the position a stream starts at is the session
//   clock (sessiontime.h) modulo the stream's length instead, so every
//   machine playing that station plays the same moment of it, in the same car
//   or not. Nothing about it goes on the wire.
//
// The user-track player (9) is neither: its files are whatever each player
// keeps in his own mp3 folder, so its position stays the engine's. The
// station number still travels, so everybody in the car is on their own user
// tracks, and a machine with none is left to the engine's own fallback.
//
// Pure arithmetic, so clienttest runs all of it; game/radio.cpp is the engine
// side.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {

// cMusicManager's track byte for "nothing" (addresses.h, MUSIC_NO_TRACK).
constexpr uint8_t RADIO_NO_TRACK = 0xC5;

// Streams played on the session clock: the nine stations and the police
// scanner, each one file of fixed length on every install. Not 9, the user
// tracks, nor the ambience, the announcements or the cutscene tracks that
// share the same stream numbers above them.
constexpr bool RadioTrackOnSessionClock(uint32_t track) {
	return track <= 8 || track == RADIO_STATION_POLICE;
}

// Where in a stream of `lengthMs` the session clock says to be.
constexpr uint32_t RadioPositionMs(uint32_t sessionMs, uint32_t lengthMs) {
	return lengthMs ? sessionMs % lengthMs : 0;
}

// How far the stream is ahead of where it should be, negative behind, taken
// around the loop: a stream 300 ms from its end is 500 ms behind a target
// 200 ms into the next pass, not a whole file ahead of it.
constexpr int32_t RadioDriftMs(uint32_t actualMs, uint32_t targetMs, uint32_t lengthMs) {
	if (lengthMs == 0)
		return 0;
	const uint32_t a     = actualMs % lengthMs;
	const uint32_t t     = targetMs % lengthMs;
	const uint32_t ahead = a >= t ? a - t : a + lengthMs - t;   // 0 .. length-1
	return ahead <= lengthMs / 2 ? static_cast<int32_t>(ahead)
	                             : -static_cast<int32_t>(lengthMs - ahead);
}

// Past this a stream is put back where the clock says. Under it the two ears
// are close enough, and a jump would be the thing somebody hears.
constexpr uint32_t RADIO_RESYNC_MS = 1000;
// How often a playing stream is compared, and how long one is left alone
// after it starts: Miles reports a position before the first buffer is out.
constexpr uint32_t RADIO_CHECK_EVERY_MS = 2000;
constexpr uint32_t RADIO_SETTLE_MS      = 3000;

// Whether a copy of a car should be made to carry the session's station.
// Nothing when the session has none yet. And not when the session says user
// tracks and this machine has none: GetCarTuning then moves the car to a
// random station of its own the moment the player is in it, which is the
// engine's answer for a machine without them, and putting 9 back would only
// have it roll again.
constexpr bool RadioCopyNeedsWrite(uint8_t session, uint8_t copy, bool userTracksHere) {
	if (!RadioStationValid(session) || session == copy)
		return false;
	return session != RADIO_STATION_USERTRACK || userTracksHere;
}

// Whether a change the music manager made to the station of the car the
// local player sits in is his to tell the session about. It is - the radio
// key, F9, a station switched off, the script's own SET_RADIO_CHANNEL on
// getting in - except the one change it makes on its own: 9 to a random
// station on a machine with no user tracks.
constexpr bool RadioChangeIsTheListeners(uint8_t before, uint8_t after, bool userTracksHere) {
	if (before == after || !RadioStationValid(after))
		return false;
	return before != RADIO_STATION_USERTRACK || userTracksHere;
}

// Whether the car the local player drives, which the session has no station
// for, gives it the one it has. The driver's machine is the car's; a
// passenger waits to be told, so that his copy's roll never outvotes the
// driver's.
constexpr bool RadioShouldSeed(uint8_t session, bool weDrive, uint8_t copy) {
	return session == RADIO_STATION_UNKNOWN && weDrive && RadioStationValid(copy);
}

// Whether the music manager is already playing what a car's radio says, so a
// station written onto the car the local player sits in needs no retune.
// `tuning` is what GetCarTuning would answer - the scanner in a police car,
// else the car's station - and `nextTrack` the stream it has on. Off is the
// ambience, city or water (11 or 12), or nothing at all between the two.
constexpr bool RadioListenerOn(uint8_t tuning, uint8_t nextTrack) {
	if (tuning < RADIO_STATION_OFF)
		return nextTrack == tuning;
	return nextTrack == RADIO_STATION_OFF || nextTrack == RADIO_STATION_OFF + 1 ||
	       nextTrack == RADIO_NO_TRACK;
}

// Watches a playing stream and says when it has wandered from the clock.
// `steady` is the music manager in the game, the local player settled in a
// car, the stream playing the station and nothing else on the way in or out
// of it - no retune counting down, no announcement.
class RadioDriftWatch {
public:
	void Reset() { *this = RadioDriftWatch{}; }

	// A stream just started, by the engine or by us: its position is ours by
	// construction, and Miles needs a moment before it reports it.
	void NoteStart(uint32_t nowMs) {
		m_steadySinceMs = nowMs;
		m_haveSteady    = true;
	}

	// True when the stream should be started again where the clock says.
	bool Tick(uint32_t nowMs, bool steady, uint32_t track, uint32_t actualMs, uint32_t targetMs,
	          uint32_t lengthMs) {
		if (!steady || track != m_track || !m_haveSteady) {
			m_track = steady ? track : NO_TRACK;
			NoteStart(nowMs);
			return false;
		}
		if (nowMs - m_steadySinceMs < RADIO_SETTLE_MS || nowMs - m_lastCheckMs < RADIO_CHECK_EVERY_MS)
			return false;
		m_lastCheckMs      = nowMs;
		m_lastDriftMs      = RadioDriftMs(actualMs, targetMs, lengthMs);
		const int32_t over = m_lastDriftMs < 0 ? -m_lastDriftMs : m_lastDriftMs;
		if (static_cast<uint32_t>(over) <= RADIO_RESYNC_MS)
			return false;
		NoteStart(nowMs);
		return true;
	}

	// What the last comparison measured. For the log.
	int32_t LastDriftMs() const { return m_lastDriftMs; }

private:
	static constexpr uint32_t NO_TRACK = 0xFFFFFFFFu;

	uint32_t m_track         = NO_TRACK;
	uint32_t m_steadySinceMs = 0;
	uint32_t m_lastCheckMs   = 0;
	int32_t  m_lastDriftMs   = 0;
	bool     m_haveSteady    = false;
};

} // namespace coopiii
