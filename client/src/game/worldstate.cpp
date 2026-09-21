#include "worldstate.h"

#include "addresses.h"
#include "log.h"

namespace coopiii::game {
namespace {

using SetGameClockFn = void(__cdecl *)(int hour, int minute);

// Whether there is a world to read at all.
//
// gGameState is the variable WinMain switches on, and GS_PLAYING_GAME is the
// only state where CGame::Process runs a full frame. CoopIII's frame pump
// already only fires from inside CGame::Process, so this is belt and braces
// - but the clock globals hold stale values from the previous session
// through a load, and reporting one of those as the session's time of day
// would drag every other player's clock to it.
bool WorldIsUp() {
	return Global<uint32_t>(gGameState) == GS_PLAYING_GAME;
}

} // namespace

bool SampleWorld(WorldState &out) {
	if (!WorldIsUp())
		return false;

	out.hour   = Global<uint8_t>(CClock__ms_nGameClockHours);
	out.minute = Global<uint8_t>(CClock__ms_nGameClockMinutes);

	// int16 on the engine side, 0..3 in practice. Anything else means we are
	// reading a global the engine has not set up yet, and it is better to
	// report nothing than to make everyone else's sky match it.
	const int16_t newType = Global<int16_t>(CWeather__NewWeatherType);
	const int16_t oldType = Global<int16_t>(CWeather__OldWeatherType);
	if (newType < 0 || newType >= WEATHER_TOTAL || oldType < 0 ||
	    oldType >= WEATHER_TOTAL)
		return false;

	out.weather    = static_cast<uint8_t>(newType);
	out.weatherOld = static_cast<uint8_t>(oldType);
	return true;
}

void ApplyWorldTime(uint8_t hour, uint8_t minute) {
	if (!WorldIsUp() || hour > 23 || minute > 59)
		return;

	// The engine's own setter rather than two byte writes. It zeroes
	// ms_nGameClockSeconds and rebases ms_nLastClockTick, so CClock::Update
	// starts timing the next game minute from now instead of finishing the
	// one that was already part-way through.
	Func<SetGameClockFn>(CClock__SetGameClock)(hour, minute);
}

void ApplyWorldWeather(uint8_t weather, uint8_t weatherOld) {
	if (!WorldIsUp() || weather >= WEATHER_TOTAL || weatherOld >= WEATHER_TOTAL)
		return;

	// CWeather::ForceWeatherNow would do two thirds of this, but it sets old
	// and new to the same type, and the host is usually part-way through a
	// blend between two different ones. So both ends go across the wire and
	// both get written here. The position within the blend does not: it is
	// CClock::GetMinutes()/60, recomputed at the top of CWeather::Update on
	// every frame, so a matching clock already makes it match.
	Global<int16_t>(CWeather__OldWeatherType) = static_cast<int16_t>(weatherOld);
	Global<int16_t>(CWeather__NewWeatherType) = static_cast<int16_t>(weather);

	// And pin it. Without this the local rotation picks its own next type
	// when the hour rolls over, which would fight the session for the second
	// or so before the next world packet arrives - a visible flicker of the
	// wrong sky, once every game hour.
	Global<int16_t>(CWeather__ForcedWeatherType) = static_cast<int16_t>(weather);
}

void ReleaseWorldWeather() {
	if (!WorldIsUp())
		return;
	Global<int16_t>(CWeather__ForcedWeatherType) = WEATHER_RANDOM;
}

void AddWorldToBridge(WorldBridge &bridge) {
	bridge.SampleWorld         = &SampleWorld;
	bridge.ApplyWorldTime      = &ApplyWorldTime;
	bridge.ApplyWorldWeather   = &ApplyWorldWeather;
	bridge.ReleaseWorldWeather = &ReleaseWorldWeather;
	Log("bridge: time of day and weather follow the session host");
}

} // namespace coopiii::game
