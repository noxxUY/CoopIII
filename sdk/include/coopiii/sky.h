// Whose game the session's time of day and sky are, and how a machine that
// follows them writes them into its own CWeather.
//
// docs/protocol.md 2.7 and docs/missions.md 13. Pure, so the server, the
// client and their tests work out the same answer from the same rule.
#pragma once

#include "protocol.h"

#include <cstdint>

namespace coopiii {

// The session's clock and sky are one player's game: the host's, and while
// the session's mission runs, its owner's.
//
// The mission's script runs on the owner's machine and nowhere else. Its
// SET_TIME_OF_DAY (00C0), FORCE_WEATHER (01B5), FORCE_WEATHER_NOW (01B6) and
// RELEASE_WEATHER (01B7) land there alone: a participant's own copy of the
// mission is held at its start gate and never runs, and the replay list
// (client/src/game/replay.h) leaves all four off on purpose, so nothing but
// the world packet ever moves a participant's sky. With the host in charge
// regardless, a mission owned by anybody else had its clock put back to the
// host's within a second of setting it.
//
// `missionOwner` is the running mission's owner, INVALID_PLAYER when none
// runs. The server takes C_WorldState from this player alone, and a client
// works the same answer out from S_MissionState and the host byte it already
// has, so the holder itself is never on the wire.
constexpr uint8_t SkyHolder(uint8_t hostId, uint8_t missionOwner) {
	return missionOwner < MAX_PLAYERS ? missionOwner : hostId;
}

// The same, as a client sees it. A mission this machine has just launched
// makes it the holder before the server has answered: the mission sets the
// clock on its first frame, and a world packet the server sent before it had
// heard of the mission would put it straight back. The server drops our
// reports until it has; C_MissionStarted goes ahead of them on the same
// channel, so that is never longer than the one packet.
constexpr uint8_t SkyHolderHere(uint8_t hostId, uint8_t missionOwner, uint8_t localId,
                                bool launchPendingHere) {
	return missionOwner >= MAX_PLAYERS && launchPendingHere && localId < MAX_PLAYERS
	           ? localId
	           : SkyHolder(hostId, missionOwner);
}

// What a follower writes into CWeather: the type being blended out of, the one
// being blended into, and what ForcedWeatherType is pinned to.
struct SkyWrite {
	uint8_t weatherOld = 0;
	uint8_t weather    = 0;
	uint8_t forced     = 0;
};

// CWeather blends from the old type to the new one across the game hour, and
// when the minutes go back to 0 it moves new into old and takes the next new
// one from ForcedWeatherType (CWeather::Update, 0x00522C44). A follower's
// clock is let drift up to three game minutes from the holder's, so around
// the top of every hour one of the two has turned over and the other has not.
// Copying the pair as it came then puts the follower at the wrong end of the
// blend for up to three seconds an hour: 58 minutes into a sky the holder has
// only just started, or 2 minutes into the one it has just left.
//
// `session*` is the world packet, `local*` this machine's clock and old type,
// after any correction the packet made to the clock.
inline SkyWrite FollowSky(uint8_t sessionHour, uint8_t sessionOld, uint8_t sessionNew,
                          uint8_t localHour, uint8_t localOld) {
	const int ahead = (int(sessionHour % 24) - int(localHour % 24) + 24) % 24;
	SkyWrite  w;
	if (ahead == 1) {
		// The holder has turned over and we have not. We are still blending
		// into what is its old type now, and our own turn-over takes its new
		// one off the pin.
		w.weatherOld = localOld;
		w.weather    = sessionOld;
		w.forced     = sessionNew;
	} else if (ahead == 23) {
		// We have turned over and the holder has not. Its new type is where
		// our hour starts; what comes after it isn't known until it turns
		// over as well.
		w.weatherOld = sessionNew;
		w.weather    = sessionNew;
		w.forced     = sessionNew;
	} else {
		w.weatherOld = sessionOld;
		w.weather    = sessionNew;
		w.forced     = sessionNew;
	}
	return w;
}

} // namespace coopiii
