# The radio

How GTA III 1.0's car radio works, read out of the retail `gta3.exe`, and
what CoopIII does so that everybody in a car hears the same station at the
same moment. Every address below is in `client/src/game/addresses.h` under
"the car radio", and `tools/clienttest/radio.cpp` checks each one against the
exe when `COOPIII_GTA3_EXE` names one. re3's `MusicManager.cpp` was the map;
where the two disagree the exe is what is written here.

The wire side is [protocol.md](protocol.md) §1.33.

## 1. The pieces

- **`cMusicManager MusicManager`** at `0x008F3964`. Found through the station
  names: `FEA_FM0` (`0x0060D3E8`) is pushed once, at `0x0057E80B`, inside
  `DisplayRadioStationName` (`0x0057E6D0`), whose one caller loads
  `mov ecx,8F3964h` first (`0x0048E3FB`).
- **`cSampleManager SampleManager`** at `0x007341E0`, the Miles wrapper. The
  music uses one stream, stream 0, for everything: the radio, the ambience,
  the announcements, the front end's tracks and the cutscene tracks.
- **`cMusicManager::Service`** (`0x0057D440`), called once a frame from the
  tail of `cAudioManager::Service` (`0x0057A2FA`), which is `DMAudio.Service`,
  the call right after `CGame::Process`. Cutscene mode returns at once. Game
  mode goes to `ServiceGameMode` (`0x0057D690`), which is the whole car radio.

Stream numbers 0-8 are the nine stations (Head Radio, Double Clef, Jah Radio,
Rise FM, Lips 106, Game FM, MSX FM, Flashback, Chatterbox), 9 the user-track
player, 10 the police scanner loop, 11 and 12 the city and water ambience,
then the announcements and the cutscene tracks, 196 (`0C4h`) in all.
`0C5h` is "nothing".

## 2. A station is one long file

Each station is a single stream file (`AUDIO\HEAD.WAV` and so on; the name
table is at `0x006034DC`, 25 bytes a row). `StartStreamedFile(file, posMs,
stream)` (`0x00567D80`) opens it, sets the loop count to 1 and seeks to
`posMs` with `AIL_set_stream_ms_position` (`0x005680A4`). Positions are
milliseconds. The lengths are measured off the files themselves when the
sample manager starts (`nStreamLength[]`, `0x0087FEF0`, read by
`GetStreamedFileLength`, `0x00568270`), so two installs with the same audio
agree on every length.

The illusion of a radio that plays on without you is three numbers per
stream in `m_aTracks[196]` at `+0x0C`, 12 bytes each: length, position, and
the pause-mode time (`CTimer::m_snTimeInMillisecondsPauseMode`) the position
was taken at.

- **At start-up**, `cMusicManager::Initialise` (`0x0057CF70`) seeds every
  position from the local date: `time()`, `localtime()`, any zero field
  replaced from the audio manager's random table, then
  `sec⁴ · min² · hour² · mday · mon · year · wday · yday`, times
  `m_anRandomTable[i % 5]`, modulo the length. So each machine starts every
  station somewhere different.
- **When a stream stops** - getting out, switching station, a cutscene, the
  menu - its position is read back off Miles (`GetStreamedFilePosition`,
  `0x00568130`) and stored with the time.
- **When a stream starts**, `GetTrackStartPos` (`0x0057E450`) returns the
  stored position plus the time since, **capped at 90 s** (`cmp edx,15F90h`
  at `0x0057E474`), modulo the length. Leave a station for ten minutes and it
  has moved on a minute and a half.
- **When a stream runs out** - Miles plays it once - `ServiceTrack`
  (`0x0057E100`) starts it again at position 0 on the next 2 s service tick
  (`0x0057E129`).

## 3. Which station a car is on

`CVehicle::m_nRadioStation` (`+0x229`, a byte).

- **A new car** rolls it: `CGeneral::GetRandomNumber() % 10` in the
  constructor (`0x00550F84`). Ten, not nine as re3 has it, so a car can come
  up on the user tracks.
- **A driver who is not the player** moves it when he takes the seat:
  `CPed::SetRadioStation` (`0x004D7BC0`, called from `0x004E0782` and
  `0x004E187C`) keeps the car's station if it is one of four his model's radio
  category likes (table `0x005F8714`, ten rows of four), and otherwise picks
  one of the four - or, one time in sixteen with user tracks present, the user
  tracks. This is the "default station per model", and it is per ped model,
  not per car model. It returns at once for the player's own ped.
- **Getting in**: `ServiceGameMode` sees `PlayerInCar()` (`0x0057E4B0`) go
  true with the previous frame's false, asks `GetCarTuning` (`0x0057E530`)
  for the car's station and `ChangeRadioChannel` (`0x0057E130`) starts it at
  `GetTrackStartPos`. `DisplayRadioStationName` shows the name again, because
  `m_bPlayerInCar` is set and `m_bPreviousPlayerInCar` is not (`0x0057E750`).
  `GetCarTuning` answers 10 for a police car, and turns 9 into a random
  station when this machine has no user tracks (`0x0057E567`).
- **Cars with no radio**: `PlayerInCar` is false in the fire engine,
  ambulance, Mr Whoopee, Predator, train, Speeder, Reefer and Ghost (table
  `0x0060D488`), and while being dragged out, getting out or arrested.
- **Police cars** - FBI car, police, Enforcer, Predator, Rhino, Barracks
  (`UsesPoliceRadio`, `0x0057E6A0`, table `0x0060D560`) - play the scanner
  loop, stream 10, whatever the byte says, and take no radio key.
- **The radio key**: the pad's change-station test (`0x00493870`) sets
  `gRetuneCounter` (`0x00650B84`) to 30 frames and adds one to
  `gNumRetunePresses` (`0x00650B80`). While the counter runs the station crackles at a quarter
  volume; when it runs out, `GetNextCarTuning` (`0x0057E5A0`) adds the presses
  to the car's byte, wrapping through "off" (11), and `ChangeRadioChannel`
  starts the new station at its start position.
- **F9** (`ControlsManager.GetIsKeyboardKeyJustDown(3F1h)` at `0x0057D7FE`,
  the key re3 names `rsF9`) jumps straight to the user tracks and writes 9
  into the car.
- **Getting out**: the station playing is remembered in `m_nRadioInCar`
  (`+0x958`) and the ambience takes the stream.
- **The script**: `SET_RADIO_CHANNEL` sets `m_bRadioSetByScript` (`+0x94F`),
  the station (`+0x950`) and a position (`+0x954`), which the next getting-in
  plays and writes into the car.

## 4. What else uses the stream

- **Cutscenes**: `ChangeMusicMode(CUTSCENE)` stores the station's position and
  stops it; the cutscene's track is preloaded and played; `Service` does
  nothing else in that mode. Back in the game the player is "getting in" again
  and the station starts at `GetTrackStartPos`.
- **Mission audio** is not the stream. It is the audio manager's own sample,
  and while it plays the radio is ducked to 25 (the `cmp byte [885AADh],1`
  ahead of every volume write, re3's `ShouldDuckMissionAudio`).
- **Announcements** (`PlayRadioAnnouncement`, which the script calls, e.g.
  at `0x0044981D` and `0x0044984E`) are stream tracks the steady in-car
  branch plays in place of the station and then goes back through
  `ChangeRadioChannel`.
- **The police dispatcher's voice** ("suspect last seen...") is
  `cPoliceRadio`, sample-based and driven by this machine's own wanted level.
  Not part of this, and not examined in the exe.
- **The front end**: the audio menu's radio preview plays the station through
  `ServiceFrontEndMode`'s own `GetTrackStartPos` call (`0x0057D59C`).

## 5. What CoopIII does

Three call sites pointed at CoopIII, no detour (`client/src/game/radio.cpp`);
the rules are pure and tested in `client/src/radiosync.h`.

1. **The start position is the session clock.** `ChangeRadioChannel`'s call
   to `GetTrackStartPos` (`0x0057E19E`) answers `session ms % length` for the
   nine stations and the scanner, and `ServiceTrack`'s restart
   (`0x0057E129`) seeks to the same instead of 0. The session clock is the
   server's, estimated by every client (`client/src/sessiontime.h`), the one
   the trains, planes, lights and lift bridge already run on. So a station is
   at the same moment on every machine, in the same car or not.
2. **Drift is corrected, rarely.** Around `cMusicManager::Service`
   (`0x0057A2FA`), a steadily playing station is compared with the clock every
   2 s, after 3 s to settle, taking the difference around the loop; past
   **1 s** it is restarted where the clock says. Under that nothing is touched,
   so nobody hears a correction smaller than the error.
3. **The station is the session's.** Every copy of a session car carries the
   session's station; anybody in the car may change it; the driver's copy
   gives the car its first one. Protocol §1.33 has the rules. When a station
   is written onto the car the local player sits in, or the drift is too
   large, the music manager is put back to the moment of getting in -
   `m_nNextTrack = 0C5h`, which makes `Service` stop the stream
   (`0x0057D4DA`), and `m_bPlayerInCar = 0` - and its own getting-in path tunes
   the car's station, shows its name and starts it at the clock. CoopIII
   never starts or stops a stream itself.
4. **The listener's change is noticed**, not predicted: the car's byte is read
   either side of `Service`, and a difference is the radio key, F9 or off -
   except 9 turning into a station on a machine without user tracks, which is
   the engine's.

Nothing happens without a session clock (not connected), so single player is
untouched. **The user tracks stay local**: each player's mp3 folder is his
own, so their position is the engine's, and a machine without any lets the
engine choose, as it does in single player. Cutscenes, mission audio, the
announcements, the dispatcher and the menu preview are not touched.

## 6. Not proven, or left out

- Nothing has run in the game yet. What an in-game session has to confirm:
  that two machines really sound the same (Miles' reported position is
  measured after its buffer, and both machines buffer alike, but that is an
  expectation); that the retune after a remote change shows the name and
  crackles no more than getting in does; and how often the 1 s resync fires.
- An announcement or a script's `SET_RADIO_CHANNEL` position plays as the
  engine plays it; only the station it leaves the car on travels.
- The retune presses a player has queued when another player's change
  arrives are added to the new station, not the old one.
- A copy is put back on the session's station at the 25 Hz send rate, so a
  remote driver's ped sitting down can reroll it for up to 40 ms; if the local
  player is in that car the music manager may start the wrong station first
  and retune a frame later.
