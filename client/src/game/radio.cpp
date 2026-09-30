#include "radio.h"

#include "addresses.h"
#include "clock.h"
#include "leadcheck.h"
#include "log.h"
#include "radiosync.h"
#include "sessionclock.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {
namespace {

using ServiceFn     = void(__thiscall *)(void *music);
using StartPosFn    = uint32_t(__thiscall *)(void *music, uint8_t track);
using StartStreamFn = bool(__thiscall *)(void *samples, uint8_t file, uint32_t posMs, uint8_t stream);
using StreamFn      = bool(__thiscall *)(void *samples, uint8_t stream);
using PositionFn    = int32_t(__thiscall *)(void *samples, uint8_t stream);
using UserTracksFn  = bool(__thiscall *)(void *samples);
using PoliceFn      = bool(__thiscall *)(void *music, void *vehicle);
using FindCarFn     = void *(__cdecl *)();
using GetCarFn      = void *(__cdecl *)(int32_t handle);
using GetCarRefFn   = int32_t(__cdecl *)(void *vehicle);

bool g_serviceRedirected  = false;
bool g_startPosRedirected = false;
bool g_restartRedirected  = false;

RadioDriftWatch g_drift;

// Client wrote a station onto the car the local player sits in. Taken up on
// the next Service, before the music manager runs.
bool g_retuneAsked = false;

// The station our music manager last moved the car we sit in to, for Client.
bool    g_changePending = false;
int32_t g_changeHandle  = -1;
uint8_t g_changeStation = 0;

// Once each in the log.
bool g_saidPosition = false;
bool g_saidRestart  = false;
bool g_saidRetune   = false;
bool g_saidChange   = false;
uint32_t g_resyncs  = 0;

void *Music() { return Ptr<void>(MusicManager); }
void *Samples() { return Ptr<void>(SampleManager); }

uint8_t &MusicByte(size_t offset) { return Field<uint8_t>(Music(), offset); }

uint32_t TrackLength(uint8_t track) {
	if (track >= MUSIC_STREAMED_SOUNDS)
		return 0;
	return Field<uint32_t>(Music(), offs::MUSIC_TRACKS + track * offs::MUSIC_TRACK_STRIDE +
	                                    offs::MUSIC_TRACK_LENGTH);
}

void *PlayerCar() { return Func<FindCarFn>(FindPlayerVehicle)(); }

bool UserTracksHere() {
	return Func<UserTracksFn>(cSampleManager__IsMP3RadioChannelAvailable)(Samples());
}

bool UsesPoliceRadio(void *car) {
	return Func<PoliceFn>(cMusicManager__UsesPoliceRadio)(Music(), car);
}

// What GetCarTuning would answer for the car, without its side effect.
uint8_t TuningOf(void *car) {
	return UsesPoliceRadio(car) ? RADIO_STATION_POLICE
	                            : Field<uint8_t>(car, offs::VEH_RADIO_STATION);
}

// The music manager in the game with the local player settled in a car: in it
// last frame and the frame before, and no announcement on. The only state a
// retune below is for; getting in asks GetCarTuning anyway.
bool SettledInCar() {
	return MusicByte(offs::MUSIC_IS_INITIALISED) != 0 && MusicByte(offs::MUSIC_DISABLED) == 0 &&
	       MusicByte(offs::MUSIC_MODE) == MUSICMODE_GAME &&
	       MusicByte(offs::MUSIC_PLAYER_IN_CAR) != 0 &&
	       MusicByte(offs::MUSIC_PREVIOUS_PLAYER_IN_CAR) != 0 &&
	       MusicByte(offs::MUSIC_ANNOUNCEMENT_IN_PROGRESS) == 0;
}

// Takes the music manager back to the moment the player got in. m_nNextTrack
// 0C5h makes this Service stop the stream and return (0x0057D4DA); with
// m_bPlayerInCar clear, the next ServiceGameMode sees him arrive, asks
// GetCarTuning for the car's station and ChangeRadioChannel starts it where
// our GetTrackStartPos says - the engine's own way into a car, name on screen
// and all. Nothing here stops or starts a stream itself.
void Retune() {
	MusicByte(offs::MUSIC_NEXT_TRACK)    = MUSIC_NO_TRACK;
	MusicByte(offs::MUSIC_PLAYER_IN_CAR) = 0;
}

void BeforeService(void *car, uint32_t session) {
	if (!car || !SettledInCar()) {
		g_retuneAsked = false;
		g_drift.Tick(WallClock::NowMs(), false, 0, 0, 0, 0);
		return;
	}

	const uint8_t next = MusicByte(offs::MUSIC_NEXT_TRACK);
	if (g_retuneAsked) {
		g_retuneAsked = false;
		const uint8_t tuning = TuningOf(car);
		if (!RadioListenerOn(tuning, next)) {
			if (!g_saidRetune) {
				g_saidRetune = true;
				Log("radio: the session put the car we are in on station %u while we "
				    "listened to %u; tuning in the way the engine does on getting in "
				    "(said once)",
				    tuning, next);
			}
			Retune();
			return;
		}
	}

	const bool steady = MusicByte(offs::MUSIC_PLAYING_TRACK) == next &&
	                    RadioTrackOnSessionClock(next) && Global<int32_t>(gRetuneCounter) == 0 &&
	                    Func<StreamFn>(cSampleManager__IsStreamPlaying)(Samples(), 0);
	const uint32_t length = steady ? TrackLength(next) : 0;
	const uint32_t actual =
	    length ? static_cast<uint32_t>(
	                 Func<PositionFn>(cSampleManager__GetStreamedFilePosition)(Samples(), 0))
	           : 0;
	const uint32_t target = RadioPositionMs(session, length);
	if (!g_drift.Tick(WallClock::NowMs(), length != 0, next, actual, target, length))
		return;

	// Rare by design, so every one is worth a line until it is plainly not.
	if (++g_resyncs <= 8)
		Log("radio: station %u is %d ms from the session clock (%u against %u of %u); "
		    "starting it again where the clock says",
		    next, g_drift.LastDriftMs(), actual, target, length);
	Retune();
}

void __fastcall ServiceOnSessionClock(void *music, void * /*edx*/) {
	uint32_t session = 0;
	const bool onClock = SessionClockNow(session);
	void *const car = PlayerCar();

	if (onClock) {
		BeforeService(car, session);
	} else {
		g_retuneAsked = false;
		g_drift.Reset();
	}

	const uint8_t before = car ? Field<uint8_t>(car, offs::VEH_RADIO_STATION) : 0;
	Func<ServiceFn>(cMusicManager__Service)(music);
	if (!onClock || !car || PlayerCar() != car)
		return;

	const uint8_t after = Field<uint8_t>(car, offs::VEH_RADIO_STATION);
	if (!RadioChangeIsTheListeners(before, after, UserTracksHere()))
		return;
	g_changePending = true;
	g_changeHandle  = Func<GetCarRefFn>(CPools__GetVehicleRef)(car);
	g_changeStation = after;
	if (!g_saidChange) {
		g_saidChange = true;
		Log("radio: our radio moved the car we are in from station %u to %u; the "
		    "session hears it (said once)",
		    before, after);
	}
}

uint32_t __fastcall StartPosOnSessionClock(void *music, void * /*edx*/, uint32_t track) {
	const uint8_t t       = static_cast<uint8_t>(track);
	uint32_t      session = 0;
	if (RadioTrackOnSessionClock(t) && SessionClockNow(session)) {
		const uint32_t length = TrackLength(t);
		if (length != 0) {
			const uint32_t pos = RadioPositionMs(session, length);
			g_drift.NoteStart(WallClock::NowMs());
			if (!g_saidPosition) {
				g_saidPosition = true;
				Log("radio: station %u starts %u ms into its %u ms on the session clock, "
				    "not where this machine left it (said once)",
				    t, pos, length);
			}
			return pos;
		}
	}
	return Func<StartPosFn>(cMusicManager__GetTrackStartPos)(music, t);
}

bool __fastcall RestartOnSessionClock(void *samples, void * /*edx*/, uint32_t file, uint32_t posMs,
                                      uint32_t stream) {
	const uint8_t f       = static_cast<uint8_t>(file);
	uint32_t      session = 0;
	if (static_cast<uint8_t>(stream) == 0 && RadioTrackOnSessionClock(f) &&
	    SessionClockNow(session)) {
		const uint32_t length = TrackLength(f);
		if (length != 0) {
			posMs = RadioPositionMs(session, length);
			g_drift.NoteStart(WallClock::NowMs());
			if (!g_saidRestart) {
				g_saidRestart = true;
				Log("radio: station %u ran to the end of its file and goes on %u ms in, "
				    "on the session clock, rather than from the top (said once)",
				    f, posMs);
			}
		}
	}
	return Func<StartStreamFn>(cSampleManager__StartStreamedFile)(
	    samples, f, posMs, static_cast<uint8_t>(stream));
}

// Points the call at `site` at `to`, only while it still calls `from`.
bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

// ---- the bridge -------------------------------------------------------------

void *CarAt(int32_t handle) {
	return handle < 0 ? nullptr : Func<GetCarFn>(CPools__GetVehicle)(handle);
}

bool ReadCarRadio(int32_t handle, uint8_t &station) {
	void *const car = CarAt(handle);
	if (!car)
		return false;
	station = Field<uint8_t>(car, offs::VEH_RADIO_STATION);
	return true;
}

void WriteCarRadio(int32_t handle, uint8_t station) {
	void *const car = CarAt(handle);
	if (!car)
		return;
	Field<uint8_t>(car, offs::VEH_RADIO_STATION) = station;
	if (car == PlayerCar())
		g_retuneAsked = true;
}

bool DrainLocalRadioChange(int32_t &handle, uint8_t &station) {
	if (!g_changePending)
		return false;
	g_changePending = false;
	handle          = g_changeHandle;
	station         = g_changeStation;
	return true;
}

bool RadioUserTracksHere() { return UserTracksHere(); }

} // namespace

bool InstallRadioSync() {
	const auto ours = [](auto fn) { return reinterpret_cast<uintptr_t>(fn); };
	if (!g_serviceRedirected)
		g_serviceRedirected =
		    Redirect(MUSIC_SERVICE_CALL, cMusicManager__Service, ours(&ServiceOnSessionClock));
	if (!g_startPosRedirected)
		g_startPosRedirected = Redirect(CHANGE_RADIO_CHANNEL_START_POS,
		                                cMusicManager__GetTrackStartPos,
		                                ours(&StartPosOnSessionClock));
	if (!g_restartRedirected)
		g_restartRedirected = Redirect(SERVICE_TRACK_RESTART, cSampleManager__StartStreamedFile,
		                               ours(&RestartOnSessionClock));

	if (g_serviceRedirected)
		Log("radio: cMusicManager::Service at 0x%08X comes through us; a car's station "
		    "is the session's",
		    static_cast<unsigned>(MUSIC_SERVICE_CALL));
	else
		Log("radio: FAILED to redirect the call to cMusicManager::Service at 0x%08X; "
		    "everybody in a car hears his own copy's station and the dial stays his own",
		    static_cast<unsigned>(MUSIC_SERVICE_CALL));
	if (g_startPosRedirected)
		Log("radio: a station starts in a car on the session clock (0x%08X)",
		    static_cast<unsigned>(CHANGE_RADIO_CHANNEL_START_POS));
	else
		Log("radio: FAILED to redirect GetTrackStartPos at 0x%08X; every machine starts "
		    "a station where it left it, so the same station is a different song",
		    static_cast<unsigned>(CHANGE_RADIO_CHANNEL_START_POS));
	if (g_restartRedirected)
		Log("radio: a station that runs out goes on on the session clock (0x%08X)",
		    static_cast<unsigned>(SERVICE_TRACK_RESTART));
	else
		Log("radio: FAILED to redirect ServiceTrack's restart at 0x%08X; a station that "
		    "reaches the end of its file starts from the top here, and the resync puts "
		    "it back a few seconds later",
		    static_cast<unsigned>(SERVICE_TRACK_RESTART));
	return g_serviceRedirected && g_startPosRedirected && g_restartRedirected;
}

void RemoveRadioSync() {
	const auto ours = [](auto fn) { return reinterpret_cast<uintptr_t>(fn); };
	if (g_serviceRedirected &&
	    Redirect(MUSIC_SERVICE_CALL, ours(&ServiceOnSessionClock), cMusicManager__Service))
		g_serviceRedirected = false;
	if (g_startPosRedirected &&
	    Redirect(CHANGE_RADIO_CHANNEL_START_POS, ours(&StartPosOnSessionClock),
	             cMusicManager__GetTrackStartPos))
		g_startPosRedirected = false;
	if (g_restartRedirected &&
	    Redirect(SERVICE_TRACK_RESTART, ours(&RestartOnSessionClock),
	             cSampleManager__StartStreamedFile))
		g_restartRedirected = false;
	g_retuneAsked   = false;
	g_changePending = false;
	g_drift.Reset();
}

void AddRadioToBridge(WorldBridge &bridge) {
	bridge.ReadCarRadio          = &ReadCarRadio;
	bridge.WriteCarRadio         = &WriteCarRadio;
	bridge.DrainLocalRadioChange = &DrainLocalRadioChange;
	bridge.RadioUserTracksHere   = &RadioUserTracksHere;
}

} // namespace coopiii::game
