// The car radio: client/src/radiosync.h and game/radio.cpp.
//
// The rules with no engine, and - when a copy of the retail exe is handed
// over - every call site and field game/radio.cpp leans on, read back out of
// it.

#include "game/addresses.h"
#include "game/leadcheck.h"
#include "radiosync.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_radioFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_radioFailures;
}

void TestWhichStreamsFollowTheClock() {
	std::printf("\nthe radio: which streams follow the session clock\n");
	bool stations = true;
	for (uint32_t t = 0; t <= 8; ++t)
		stations = stations && RadioTrackOnSessionClock(t);
	Check(stations, "the nine stations do");
	Check(RadioTrackOnSessionClock(RADIO_STATION_POLICE), "and the police scanner");
	Check(!RadioTrackOnSessionClock(RADIO_STATION_USERTRACK),
	      "the user tracks do not: they are each player's own files");
	Check(!RadioTrackOnSessionClock(11) && !RadioTrackOnSessionClock(12),
	      "nor the city and water ambience");
	Check(!RadioTrackOnSessionClock(13) && !RadioTrackOnSessionClock(40) &&
	          !RadioTrackOnSessionClock(RADIO_NO_TRACK),
	      "nor an announcement, a cutscene's track or nothing");
}

void TestThePositionIsTheClock() {
	std::printf("\nthe radio: where in a station the clock says to be\n");
	constexpr uint32_t len = 1800000;   // half an hour
	Check(RadioPositionMs(0, len) == 0 && RadioPositionMs(1234, len) == 1234,
	      "the session clock itself, inside the first pass");
	Check(RadioPositionMs(len + 500, len) == 500, "and round again after it");
	Check(RadioPositionMs(0xFFFFFFFFu, len) == 0xFFFFFFFFu % len, "at the top of the clock too");
	Check(RadioPositionMs(5000, 0) == 0, "a stream with no length starts at the top");

	// Two machines, a second apart on their wall clocks, one session clock.
	const uint32_t offsetA = 1000000, offsetB = 1001000;
	Check(RadioPositionMs(5000 + offsetA, len) == RadioPositionMs(4000 + offsetB, len),
	      "two machines with the same session time start at the same moment");
}

void TestDriftIsTakenAroundTheLoop() {
	std::printf("\nthe radio: drift\n");
	constexpr uint32_t len = 100000;
	Check(RadioDriftMs(10500, 10000, len) == 500, "half a second ahead");
	Check(RadioDriftMs(9000, 10000, len) == -1000, "a second behind");
	Check(RadioDriftMs(99700, 200, len) == -500,
	      "300 ms before the end against 200 ms into the next pass is 500 behind");
	Check(RadioDriftMs(300, 99900, len) == 400, "and the other way round is ahead");
	Check(RadioDriftMs(123, 456, 0) == 0, "no length, no drift");
	Check(RadioDriftMs(len + 10, 10, len) == 0, "a position past the length is taken modulo it");
}

void TestTheWatchResyncsOnlyPastASecond() {
	std::printf("\nthe radio: the drift watch\n");
	constexpr uint32_t len = 600000;
	RadioDriftWatch w;
	uint32_t        now = 10000;
	Check(!w.Tick(now, true, 3, 0, 5000, len), "a stream just heard of is left to start");
	now += RADIO_SETTLE_MS - 1;
	Check(!w.Tick(now, true, 3, 0, 5000, len), "and left alone while it settles");
	now += 1;
	Check(!w.Tick(now, true, 3, 5900, 5000, len), "900 ms out is close enough");
	Check(w.LastDriftMs() == 900, "and was measured");
	now += 100;
	Check(!w.Tick(now, true, 3, 0, 5000, len), "the next look waits its turn");
	now += RADIO_CHECK_EVERY_MS;
	Check(w.Tick(now, true, 3, 3000, 5000, len), "two seconds behind is started again");
	Check(w.LastDriftMs() == -2000, "measured as behind");
	now += RADIO_CHECK_EVERY_MS;
	Check(!w.Tick(now, true, 3, 0, 5000, len),
	      "and the restart gets its own settle before it is judged");
	now += RADIO_SETTLE_MS;
	Check(!w.Tick(now, true, 3, 5000, 5000, len), "on time after it");

	now += RADIO_CHECK_EVERY_MS;
	Check(!w.Tick(now, true, 4, 0, 5000, len), "a different station starts the wait over");
	now += RADIO_CHECK_EVERY_MS;
	Check(!w.Tick(now, true, 4, 0, 5000, len), "and is not judged inside it");
	now += RADIO_SETTLE_MS;
	Check(!w.Tick(now, false, 4, 0, 5000, len), "nothing is judged while it is not steady");
	now += RADIO_SETTLE_MS;
	Check(!w.Tick(now, true, 4, 0, 5000, len), "and steady again waits again");

	RadioDriftWatch started;
	started.NoteStart(0);
	Check(!started.Tick(100, true, 2, 0, 90000, len),
	      "a stream the watch never saw start is taken as just started");

	RadioDriftWatch wrap;
	wrap.Tick(0, true, 2, 0, 0, len);
	Check(!wrap.Tick(RADIO_SETTLE_MS, true, 2, len - 300, 200, len),
	      "a stream about to run out against a target just past the top is 500 ms, not a pass");
}

void TestWhichCopiesArePutBack() {
	std::printf("\nthe radio: which copies are put back\n");
	Check(RadioCopyNeedsWrite(5, 3, false), "a copy on another station is");
	Check(!RadioCopyNeedsWrite(5, 5, false), "one on the same is not");
	Check(!RadioCopyNeedsWrite(RADIO_STATION_UNKNOWN, 3, true),
	      "nor any while the session has none");
	Check(!RadioCopyNeedsWrite(RADIO_STATION_USERTRACK, 3, false),
	      "user tracks on a machine with none: the engine's pick stands");
	Check(RadioCopyNeedsWrite(RADIO_STATION_USERTRACK, 3, true), "with some, they play");
	Check(RadioCopyNeedsWrite(RADIO_STATION_OFF, 3, false), "off is off everywhere");
	Check(!RadioCopyNeedsWrite(12, 3, true), "and a number that is not a station is not written");
}

void TestWhichChangesAreTheListeners() {
	std::printf("\nthe radio: which changes are the listener's\n");
	Check(RadioChangeIsTheListeners(3, 4, false), "the radio key");
	Check(RadioChangeIsTheListeners(3, RADIO_STATION_USERTRACK, true), "F9");
	Check(RadioChangeIsTheListeners(8, RADIO_STATION_OFF, false), "off");
	Check(RadioChangeIsTheListeners(RADIO_STATION_OFF, 0, false), "and on again");
	Check(!RadioChangeIsTheListeners(3, 3, false), "no change is none");
	Check(!RadioChangeIsTheListeners(RADIO_STATION_USERTRACK, 4, false),
	      "GetCarTuning rolling a station for user tracks we don't have is the engine's");
	Check(RadioChangeIsTheListeners(RADIO_STATION_USERTRACK, 4, true),
	      "the key moving off the user tracks we do have is his");
	Check(!RadioChangeIsTheListeners(3, 200, false), "nor is a byte that is no station");
}

void TestWhoSeedsTheStation() {
	std::printf("\nthe radio: who gives a car its first station\n");
	Check(RadioShouldSeed(RADIO_STATION_UNKNOWN, true, 4), "the driver, when the session has none");
	Check(!RadioShouldSeed(RADIO_STATION_UNKNOWN, false, 4), "never a passenger");
	Check(!RadioShouldSeed(2, true, 4), "and not over one the session has");
	Check(!RadioShouldSeed(RADIO_STATION_UNKNOWN, true, 30), "and not with a byte that is none");
}

void TestWhenTheListenerIsAlreadyOn() {
	std::printf("\nthe radio: is the listener already on it\n");
	Check(RadioListenerOn(4, 4), "the same station");
	Check(!RadioListenerOn(4, 5), "another station");
	Check(!RadioListenerOn(4, 11), "the ambience when the car has a station");
	Check(RadioListenerOn(RADIO_STATION_OFF, 11) && RadioListenerOn(RADIO_STATION_OFF, 12) &&
	          RadioListenerOn(RADIO_STATION_OFF, RADIO_NO_TRACK),
	      "off: city, water or nothing");
	Check(!RadioListenerOn(RADIO_STATION_OFF, 3), "but not a station");
	Check(RadioListenerOn(RADIO_STATION_POLICE, RADIO_STATION_POLICE), "the scanner");
}

// ---- against the real exe ------------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

// .text, .rdata and .data all sit in the file at their address less the
// image base, which is all this reads.
constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;
constexpr uint32_t kFileDelta = IMAGE_BASE;   // VA - file offset for .text and .rdata

const uint8_t *At(const std::vector<uint8_t> &img, uint32_t va) { return &img[va - kFileDelta]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	uint32_t v;
	std::memcpy(&v, At(img, va), sizeof v);
	return v;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> want) {
	size_t i = 0;
	for (uint8_t b : want)
		if (At(img, va)[i++] != b)
			return false;
	return true;
}

bool Calls(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return RelCallAt(At(img, site), site, to);
}

// Every E8 in .text that lands on `to`.
std::vector<uint32_t> CallersOf(const std::vector<uint8_t> &img, uint32_t to) {
	std::vector<uint32_t> out;
	for (uint32_t va = kTextBegin; va + 5 <= kTextEnd; ++va)
		if (Calls(img, va, to))
			out.push_back(va);
	return out;
}

// Any branch in .text whose target falls inside [from, from + len).
bool BranchInto(const std::vector<uint8_t> &img, uint32_t from, uint32_t len) {
	for (uint32_t va = kTextBegin; va + 6 <= kTextEnd; ++va) {
		const uint8_t b      = *At(img, va);
		uint32_t      target = 0;
		if (b == 0xE8 || b == 0xE9)
			target = va + 5 + Dword(img, va + 1);
		else if (b == 0x0F && (At(img, va)[1] & 0xF0) == 0x80)
			target = va + 6 + Dword(img, va + 2);
		else if ((b & 0xF0) == 0x70 || b == 0xEB)
			target = va + 2 + static_cast<uint32_t>(static_cast<int8_t>(At(img, va)[1]));
		else
			continue;
		if (target >= from && target < from + len)
			return true;
	}
	return false;
}

void TestTheRadioAgainstTheImage() {
	std::printf("\nthe radio against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "radio's addresses against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// Finding it: FEA_FM0 is pushed inside DisplayRadioStationName, and its one
	// caller hands it the MusicManager.
	Check(Bytes(img, 0x0057E80B, {0x68, 0xE8, 0xD3, 0x60, 0x00}),
	      "DisplayRadioStationName pushes FEA_FM0 (0x0060D3E8)");
	Check(std::memcmp(At(img, 0x0060D3E8), "FEA_FM0", 8) == 0, "which is the name it says");
	Check(Bytes(img, 0x0048E3FB, {0xB9, 0x64, 0x39, 0x8F, 0x00}) &&
	          Calls(img, 0x0048E400, 0x0057E6D0),
	      "its caller loads ecx with 0x008F3964 first: the MusicManager");

	// The three call sites.
	const std::vector<uint32_t> service = CallersOf(img, cMusicManager__Service);
	Check(service.size() == 1 && service[0] == MUSIC_SERVICE_CALL,
	      "cMusicManager::Service has one caller, cAudioManager::Service's tail");
	Check(Bytes(img, MUSIC_SERVICE_CALL - 5, {0xB9, 0x64, 0x39, 0x8F, 0x00}),
	      "which passes it the MusicManager in ecx");
	Check(Bytes(img, cMusicManager__Service, {0x53, 0x89, 0xCB, 0x80, 0xBB, 0x3C, 0x09, 0x00, 0x00, 0x00}),
	      "Service opens on ResetTimers' flag at +0x93C");
	Check(Bytes(img, 0x0057D472, {0x80, 0x7B, 0x02, 0x02}),
	      "and leaves at once in cutscene mode ([ebx+2] == 2)");
	Check(Bytes(img, 0x0057D4DA, {0x80, 0x7B, 0x03, 0xC5}),
	      "and stops the stream when m_nNextTrack is 0C5h, which the retune leans on");

	const std::vector<uint32_t> startPos = CallersOf(img, cMusicManager__GetTrackStartPos);
	Check(startPos.size() == 3 && startPos[0] == 0x0057D59C && startPos[1] == 0x0057DDDD &&
	          startPos[2] == CHANGE_RADIO_CHANNEL_START_POS,
	      "GetTrackStartPos: the front end, the ambience and ChangeRadioChannel");
	Check(Bytes(img, CHANGE_RADIO_CHANNEL_START_POS - 6, {0x8A, 0x43, 0x03, 0x89, 0xD9, 0x50}),
	      "ChangeRadioChannel pushes m_nNextTrack with the manager in ecx");
	Check(Bytes(img, CHANGE_RADIO_CHANNEL_START_POS + 5, {0x6A, 0x00, 0x50}) &&
	          Calls(img, 0x0057E1AF, cSampleManager__StartStreamedFile),
	      "and starts the stream at what it answers");
	Check(Bytes(img, cMusicManager__GetTrackStartPos, {0x0F, 0xB6, 0x44, 0x24, 0x04}) &&
	          Bytes(img, 0x0057E4A7, {0xC2, 0x04, 0x00}),
	      "GetTrackStartPos takes a byte off the stack and returns with ret 4");
	Check(Bytes(img, 0x0057E474, {0x81, 0xFA, 0x90, 0x5F, 0x01, 0x00}),
	      "and advances the stored position by at most 90 s");
	Check(Bytes(img, 0x0057E481, {0x8B, 0x44, 0x19, 0x10}) &&
	          Bytes(img, 0x0057E498, {0x8B, 0x4C, 0x19, 0x0C}),
	      "reading position at +0x10 and length at +0x0C of track*12");

	Check(Calls(img, SERVICE_TRACK_RESTART, cSampleManager__StartStreamedFile) &&
	          Bytes(img, SERVICE_TRACK_RESTART - 13,
	                {0x8A, 0x43, 0x03, 0xB9, 0xE0, 0x41, 0x73, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x50}),
	      "ServiceTrack restarts m_nNextTrack at position 0 on stream 0");
	Check(Bytes(img, cMusicManager__ServiceTrack,
	            {0x53, 0x89, 0xCB, 0x80, 0xBB, 0x4C, 0x09, 0x00, 0x00, 0x00}),
	      "behind the 2 s track-service flag at +0x94C");
	Check(CallersOf(img, cSampleManager__StartStreamedFile).size() == 6,
	      "StartStreamedFile has six callers, one of them ours to take");

	// A call site is only rewritten in its rel32, but nothing may land inside
	// one either.
	Check(!BranchInto(img, MUSIC_SERVICE_CALL + 1, 4) &&
	          !BranchInto(img, CHANGE_RADIO_CHANNEL_START_POS + 1, 4) &&
	          !BranchInto(img, SERVICE_TRACK_RESTART + 1, 4),
	      "no branch in the image lands inside any of the three calls");

	// The sample manager.
	Check(Bytes(img, 0x00567D8E, {0x8B, 0xAC, 0x24, 0x20, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x005680A2, {0x55, 0x50, 0xFF, 0x15, 0x68, 0xD6, 0x61, 0x00}),
	      "StartStreamedFile hands its second argument to AIL_set_stream_ms_position");
	Check(Bytes(img, 0x005680DC, {0xC2, 0x0C, 0x00}), "and returns with ret 0Ch");
	Check(Bytes(img, cSampleManager__GetStreamedFileLength,
	            {0x80, 0x79, 0x56, 0x00, 0x74, 0x0F, 0x0F, 0xB6, 0x44, 0x24, 0x04, 0x8B, 0x04,
	             0x85, 0xF0, 0xFE, 0x87, 0x00}),
	      "GetStreamedFileLength is nStreamLength[n] at 0x0087FEF0");
	Check(Bytes(img, cSampleManager__IsStreamPlaying, {0x80, 0x79, 0x56, 0x00}) &&
	          Bytes(img, 0x005682AD, {0x83, 0xF8, 0x04}),
	      "IsStreamPlaying is AIL_stream_status == 4");
	Check(Bytes(img, cSampleManager__GetStreamedFilePosition,
	            {0x53, 0x55, 0x83, 0xEC, 0x08, 0x80, 0x79, 0x56, 0x00}) &&
	          Bytes(img, 0x005681A5, {0x6A, 0x00, 0x52, 0xFF, 0x15, 0xCC, 0xD5, 0x61, 0x00}),
	      "GetStreamedFilePosition reads AIL_stream_ms_position's current ms");
	Check(Bytes(img, cSampleManager__IsMP3RadioChannelAvailable,
	            {0x83, 0xEC, 0x08, 0x83, 0x3D, 0x00, 0xCC, 0x95, 0x00, 0x00}),
	      "IsMP3RadioChannelAvailable is nNumMP3s != 0");
	Check(Calls(img, 0x0057D3BB, cSampleManager__StopStreamedFile),
	      "ChangeMusicMode stops stream 0 through StopStreamedFile");

	// cMusicManager's fields.
	Check(Bytes(img, 0x0057D6B3, {0x8A, 0x45, 0x0A, 0x88, 0x45, 0x09}) &&
	          Calls(img, 0x0057D6B9, cMusicManager__PlayerInCar) &&
	          Bytes(img, 0x0057D6BE, {0x88, 0x45, 0x0A}),
	      "ServiceGameMode: previous-in-car at +9 from +0x0A, then PlayerInCar into +0x0A");
	Check(Bytes(img, 0x0057D6C1, {0x8A, 0x45, 0x03, 0x88, 0x45, 0x04}),
	      "m_nPlayingTrack at +4 takes m_nNextTrack at +3");
	Check(Bytes(img, 0x0057D79C, {0x80, 0x7D, 0x08, 0xC4}) &&
	          Bytes(img, 0x0057D7A8, {0x80, 0x7D, 0x0B, 0x00}),
	      "the announcement at +8 and its in-progress flag at +0x0B");
	Check(Bytes(img, 0x0057D3C0, {0xC6, 0x46, 0x03, 0xC5, 0xC6, 0x46, 0x04, 0xC5}),
	      "ChangeMusicMode's nothing is 0C5h in both track bytes");
	Check(Bytes(img, 0x0057D73F, {0xC7, 0x05, 0x84, 0x0B, 0x65, 0x00, 0x1E, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x0057D749, {0xFF, 0x05, 0x80, 0x0B, 0x65, 0x00}),
	      "the radio key: 30 on gRetuneCounter, one more press on gNumRetunePresses");
	Check(Calls(img, 0x0057D7D8, cMusicManager__GetCarTuning) &&
	          Calls(img, 0x0057DC3A, cMusicManager__GetCarTuning),
	      "GetCarTuning is what getting in asks");
	Check(Bytes(img, 0x0057D8E0 - 2, {0x89, 0xE9}) &&
	          Calls(img, 0x0057D8E0, cMusicManager__ChangeRadioChannel) &&
	          Calls(img, 0x0057DC4A, cMusicManager__ChangeRadioChannel),
	      "and ChangeRadioChannel is what it tunes with, steady or getting in");

	// The car's station.
	Check(Bytes(img, 0x0057E550, {0x80, 0xBD, 0x29, 0x02, 0x00, 0x00, 0x09}),
	      "GetCarTuning compares CVehicle+0x229 with 9, the user tracks");
	Check(Bytes(img, 0x00550F84, {0x88, 0x88, 0x29, 0x02, 0x00, 0x00}) &&
	          Bytes(img, 0x00550F7F, {0x6B, 0xDB, 0x0A}),
	      "the vehicle constructor rolls it modulo ten");
	Check(Calls(img, 0x004E0782, 0x004D7BC0) && Calls(img, 0x004E187C, 0x004D7BC0),
	      "and CPed::SetRadioStation moves it when a driver sits down");
	Check(Bytes(img, 0x004D7BE2, {0x39, 0xA8, 0xA4, 0x01, 0x00, 0x00}),
	      "only for the car's own driver (m_pDriver at +0x1A4)");
	Check(Bytes(img, 0x0057E6AF, {0x83, 0xE8, 0x6B, 0x83, 0xF8, 0x10}),
	      "UsesPoliceRadio switches on the model from 107");
	const uint32_t police = 0x0057E6BE;
	bool           table  = true;
	for (uint32_t m = 107; m <= 123; ++m) {
		const bool yes = m == 107 || m == 116 || m == 117 || m == 120 || m == 122 || m == 123;
		table = table && ((Dword(img, 0x0060D560 + (m - 107) * 4) == police) == yes);
	}
	Check(table, "and says yes to the FBI car, police, Enforcer, Predator, Rhino and Barracks");
}

} // namespace

int RunRadioTests() {
	TestWhichStreamsFollowTheClock();
	TestThePositionIsTheClock();
	TestDriftIsTakenAroundTheLoop();
	TestTheWatchResyncsOnlyPastASecond();
	TestWhichCopiesArePutBack();
	TestWhichChangesAreTheListeners();
	TestWhoSeedsTheStation();
	TestWhenTheListenerIsAlreadyOn();
	TestTheRadioAgainstTheImage();
	return g_radioFailures;
}
