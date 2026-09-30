// A car's alarm, its gun, its taxi light and its handbrake: carextrasync.h,
// the Client's half of them, and - with a copy of the retail exe - every
// address game/carextras.cpp and the rest lean on, read back out of it. And
// the police car's fast wail in somebody else's traffic (game/horn.h).

#include "client.h"
#include "carextrasync.h"
#include "game/addresses.h"
#include "game/horn.h"
#include "game/siren.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_carExtrasFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_carExtrasFailures;
}

// ---- the rules ------------------------------------------------------------------

void TestWhatIsLeftOfAnAlarm() {
	std::printf("\na car's alarm: what is left of it\n");
	Check(AlarmLeftMs(0, 5000) == 0, "no alarm, nothing left");
	Check(AlarmLeftMs(AlarmUntilMs(10000, 1000), 1000) >= 10000 &&
	          AlarmLeftMs(AlarmUntilMs(10000, 1000), 1000) <= 10001,
	      "an alarm just heard has what it was sent with");
	Check(AlarmLeftMs(AlarmUntilMs(10000, 1000), 8000) >= 3000 &&
	          AlarmLeftMs(AlarmUntilMs(10000, 1000), 8000) <= 3001,
	      "and counts down on the wall clock");
	Check(AlarmLeftMs(AlarmUntilMs(10000, 1000), 11002) == 0, "to nothing");
	Check(AlarmUntilMs(0, 1000) == 0, "a stop is no alarm");
	Check(AlarmUntilMs(5000, 0xFFFFFFFFu - 4999) != 0,
	      "an alarm that ends as the clock wraps is still one");
	Check(AlarmLeftMs(AlarmUntilMs(5000, 0xFFFFFFF0u), 0xFFFFFFF0u + 1000) >= 3999,
	      "and counts down across the wrap");
	Check(AlarmLeftMs(AlarmUntilMs(60000, 0), 0) <= VEHICLE_ALARM_MS,
	      "never more than the engine sets");
}

void TestWhenTheAlarmIsNews() {
	std::printf("\na car's alarm: when the machine simulating it speaks\n");
	const uint32_t now = 100000;
	Check(AlarmNewsFor(0, 0, now) == AlarmNews::None, "silent and never told: nothing");
	Check(AlarmNewsFor(14980, 0, now) == AlarmNews::Started, "gone off: said");
	const uint32_t told = AlarmUntilMs(14980, now);
	Check(AlarmNewsFor(14000, told, now + 980) == AlarmNews::None, "counting down: nothing");
	Check(AlarmNewsFor(3000, told, now + 13000) == AlarmNews::None,
	      "a countdown a second behind the wall clock is the same alarm");
	Check(AlarmNewsFor(500, told, now + 16000) == AlarmNews::None,
	      "and so is one still going after the record ran out");
	Check(AlarmNewsFor(0, told, now + 1000) == AlarmNews::Stopped,
	      "an alarm that stopped with most of it left: said");
	Check(AlarmNewsFor(0, told, now + 14000) == AlarmNews::None,
	      "one that ran out on time: nothing");
	Check(AlarmNewsFor(15000, AlarmUntilMs(2000, now), now) == AlarmNews::Started,
	      "one with much more left than was told is a new alarm");
}

void TestWhenTheGunIsNews() {
	std::printf("\nwhere a car's gun points: when the driver speaks\n");
	Check(CarHasGun(122) && CarHasGun(97), "the tank and the fire truck have one");
	Check(!CarHasGun(116) && !CarHasGun(90) && !CarHasGun(0), "a police car does not");
	Check(AimShouldSend(false, 0, 0, 0, 1.0f, 0.1f, 5), "the first look goes out");
	Check(!AimShouldSend(true, 1.0f, 0.1f, 5, 1.001f, 0.1f, 50),
	      "a turn under a tenth of a degree does not");
	Check(AimShouldSend(true, 1.0f, 0.1f, 5, 1.01f, 0.1f, 50), "a real one does");
	Check(AimShouldSend(true, 1.0f, 0.1f, 5, 1.0f, 0.2f, 50), "and so does the cannon's tilt");
	Check(!AimShouldSend(true, 1.0f, 0.1f, 5, 1.0f, 0.1f, 5 + AIM_REFRESH_MS - 1),
	      "a still turret waits");
	Check(AimShouldSend(true, 1.0f, 0.1f, 5, 1.0f, 0.1f, 5 + AIM_REFRESH_MS),
	      "and is said again once a second");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!AimShouldSend(false, 0, 0, 0, nan, 0.1f, 5), "a NaN never goes out");
	Check(!AimFinite(1e9f, 0.0f) && AimFinite(6.28f, 0.3f), "nor nonsense");
}

// The traffic horn on a siren car: it goes out, because the replica is given
// the siren beside it and wails fast with it, as the host does.
void TestThePoliceCarsFastWailGoesOut() {
	std::printf("\nthe fast wail of a police car stuck in traffic\n");
	Check(TrafficHornOnWire(MODEL_POLICE, true, 30),
	      "a police car with its siren on and its horn timer running sends it");
	Check(TrafficHornOnWire(MODEL_AMBULAN, true, 44) && TrafficHornOnWire(MODEL_ENFORCER, true, 1),
	      "so do an ambulance and an Enforcer");
	Check(TrafficHornOnWire(MODEL_POLICE, false, 30), "a police car honking without it too");
	Check(!TrafficHornOnWire(MODEL_POLICE, true, 0), "no timer, nothing");
	Check(!TrafficHornOnWire(MODEL_MRWHOOP, false, 30), "Mr Whoopee never");
	Check(AudioSwitchesSirenForHorn(MODEL_POLICE) && !AudioSwitchesSirenForHorn(MODEL_FIRETRUK),
	      "the timer switches the police car's siren, and honks over the fire truck's");
}

// ---- the Client --------------------------------------------------------------

struct CarRec {
	uint16_t alarm[64]    = {};
	int      alarmWrites  = 0;
	float    lr[64]       = {};
	float    ud[64]       = {};
	bool     hasGun[64]   = {};
	int      aimWrites    = 0;
	int32_t  localHandle  = -1;
	int32_t  nextHandle   = 3;
};
CarRec g_car;

void RecRequestModel(uint16_t) {}
bool RecModelReady(uint16_t) { return true; }
bool RecSpawn(RemoteVehicle &v) {
	v.poolHandle = g_car.nextHandle++;
	return true;
}
int32_t RecLocalHandle() { return g_car.localHandle; }
bool RecReadAlarm(int32_t h, uint16_t &ms) {
	if (h < 0 || h >= 64)
		return false;
	ms = g_car.alarm[h];
	return true;
}
void RecWriteAlarm(int32_t h, uint16_t ms) {
	if (h < 0 || h >= 64)
		return;
	g_car.alarm[h] = ms;
	++g_car.alarmWrites;
}
bool RecReadAim(int32_t h, float &lr, float &ud) {
	if (h < 0 || h >= 64 || !g_car.hasGun[h])
		return false;
	lr = g_car.lr[h];
	ud = g_car.ud[h];
	return true;
}
void RecWriteAim(int32_t h, float lr, float ud) {
	if (h < 0 || h >= 64)
		return;
	g_car.lr[h] = lr;
	g_car.ud[h] = ud;
	++g_car.aimWrites;
}

WorldBridge CarBridge() {
	g_car = CarRec{};
	WorldBridge b;
	b.RequestModel             = &RecRequestModel;
	b.IsModelReady             = &RecModelReady;
	b.SpawnRemoteVehicle       = &RecSpawn;
	b.SampleLocalVehicleHandle = &RecLocalHandle;
	b.ReadCarAlarm             = &RecReadAlarm;
	b.WriteCarAlarm            = &RecWriteAlarm;
	b.ReadCarAim               = &RecReadAim;
	b.WriteCarAim              = &RecWriteAim;
	return b;
}

template <class T>
Message Wrap(const T &pkt, uint8_t channel = CH_EVENT) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = channel;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject       = 0;
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.minute       = 0;
	w.weather      = 0;
	w.weatherOld   = 0;
	w.hostPlayerId = INVALID_PLAYER;
	w.flags        = 0;
	return w;
}

S_VehicleSpawn Spawn(uint16_t netId, uint16_t model) {
	S_VehicleSpawn s;
	InitHeader(s, 1000);
	s.netId   = netId;
	s.modelId = model;
	s.pos     = {10.0f, 20.0f, 30.0f};
	s.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	s.colour1 = 1;
	s.colour2 = 1;
	s.health  = 1000.0f;
	s.flags   = 0;
	s.extra1  = -1;
	s.extra2  = -1;
	return s;
}

S_VehicleAlarm Alarm(uint16_t netId, uint16_t ms) {
	S_VehicleAlarm a{};
	InitHeader(a, 1000);
	a.playerId    = 1;
	a.netId       = netId;
	a.remainingMs = ms;
	return a;
}

S_VehicleAim Aim(uint16_t netId, float lr, float ud) {
	S_VehicleAim a{};
	InitHeader(a, 1000);
	a.playerId = 1;
	a.netId    = netId;
	a.gunLR    = lr;
	a.gunUD    = ud;
	return a;
}

S_EnterVehicle WeDrive(uint16_t netId, uint16_t model) {
	S_EnterVehicle e{};
	InitHeader(e, 1000);
	e.playerId     = 0;
	e.body.netId   = netId;
	e.body.seat    = 0;
	e.body.modelId = model;
	return e;
}

// Somebody else's car, whose alarm his engine set off.
void TestACopyGoesOffWithItsOwner() {
	std::printf("\na car's alarm: our copy of somebody else's\n");
	Client c;
	c.SetBridge(CarBridge());
	c.HandleMessage(Wrap(Welcome(0)));
	c.HandleMessage(Wrap(Spawn(80, 91)));
	c.Tick();
	const int32_t car = c.SessionCarHandleOf(80);
	Check(car >= 0 && car < 64, "the copy is built");

	c.CorrectCarExtrasForTest();
	Check(g_car.alarmWrites == 0, "with no alarm, nothing is written");

	c.HandleMessage(Wrap(Alarm(80, 12000)));
	c.CorrectCarExtrasForTest();
	Check(g_car.alarm[car] > 11000 && g_car.alarm[car] <= 12000,
	      "the copy is given what is left of it");
	g_car.alarm[car] = 0;   // a copy nobody drives, which the engine never counts down here
	c.CorrectCarExtrasForTest();
	Check(g_car.alarm[car] > 11000, "and keeps it every frame, whatever its engine did");
	Check(c.AlarmReportsSentForTest() == 0, "none of it is ours to report");

	c.HandleMessage(Wrap(Alarm(80, 0)));
	Check(g_car.alarm[car] == 0, "a stop is written at once");
	const int writes = g_car.alarmWrites;
	c.CorrectCarExtrasForTest();
	Check(g_car.alarmWrites == writes, "and after it nothing more");

	c.HandleMessage(Wrap(Alarm(80, 1)));
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	c.CorrectCarExtrasForTest();
	Check(g_car.alarm[car] == 0, "one that has run out is put out");
	const int after = g_car.alarmWrites;
	c.CorrectCarExtrasForTest();
	Check(g_car.alarmWrites == after, "once");

	c.HandleMessage(Wrap(Alarm(81, 5000)));
	Check(c.SessionCarHandleOf(81) < 0, "one for a car we have no row for makes none");
}

// The car we drive. Its alarm is our engine's, and it is said once.
void TestOurAlarmIsSaidOnce() {
	std::printf("\na car's alarm: the car we drive\n");
	Client c;
	c.SetBridge(CarBridge());
	c.HandleMessage(Wrap(Welcome(0)));
	g_car.localHandle = 7;
	c.HandleMessage(Wrap(WeDrive(77, 91)));
	Check(c.LocalVehicleNetId() == 77, "we drive vehicle 77");

	c.TickCarExtrasForTest();
	Check(c.AlarmReportsSentForTest() == 0, "no alarm, nothing said");

	g_car.alarm[7] = 14990;   // the driver's seat of an armed car (0x004CF3EF)
	c.TickCarExtrasForTest();
	Check(c.AlarmReportsSentForTest() == 1 && c.LastAlarmSentForTest() == 14990,
	      "it goes off and the session hears how long it has");
	g_car.alarm[7] = 14000;
	c.TickCarExtrasForTest();
	Check(c.AlarmReportsSentForTest() == 1, "said once, not every tick");
	c.CorrectCarExtrasForTest();
	Check(g_car.alarmWrites == 0, "and nothing is written onto our own car");

	g_car.alarm[7] = 0;
	c.TickCarExtrasForTest();
	Check(c.AlarmReportsSentForTest() == 2 && c.LastAlarmSentForTest() == 0,
	      "an alarm that stops with most of it left: that is said too");
}

// Taking the wheel of a car whose alarm we heard from somebody else. Our copy
// has been counting it down in step, so there is nothing to say.
void TestTakingTheWheelSaysNothing() {
	std::printf("\na car's alarm: taking the wheel of a car that is going off\n");
	Client c;
	c.SetBridge(CarBridge());
	c.HandleMessage(Wrap(Welcome(0)));
	c.HandleMessage(Wrap(Spawn(80, 91)));
	c.Tick();
	const int32_t car = c.SessionCarHandleOf(80);
	c.HandleMessage(Wrap(Alarm(80, 12000)));
	c.CorrectCarExtrasForTest();
	Check(g_car.alarm[car] > 11000, "the copy sounds");

	g_car.localHandle = car;
	c.HandleMessage(Wrap(WeDrive(80, 91)));
	Check(c.LocalVehicleNetId() == 80, "we drive it now");
	c.TickCarExtrasForTest();
	Check(c.AlarmReportsSentForTest() == 0, "and do not tell anybody it went off again");
}

// Where a tank's turret points.
void TestTheTurretFollowsItsDriver() {
	std::printf("\nwhere a car's gun points\n");
	Client c;
	c.SetBridge(CarBridge());
	c.HandleMessage(Wrap(Welcome(0)));
	c.HandleMessage(Wrap(Spawn(80, 122)));
	c.Tick();
	const int32_t tank = c.SessionCarHandleOf(80);
	c.HandleMessage(Wrap(Aim(80, 1.25f, 0.05f), CH_SNAPSHOT));
	c.CorrectCarExtrasForTest();
	Check(g_car.lr[tank] == 1.25f && g_car.ud[tank] == 0.05f,
	      "somebody else's turret turns on our copy");
	g_car.lr[tank] = 0.0f;
	c.CorrectCarExtrasForTest();
	Check(g_car.lr[tank] == 1.25f, "and stays where he left it, every frame");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	c.HandleMessage(Wrap(Aim(80, nan, 0.05f), CH_SNAPSHOT));
	c.CorrectCarExtrasForTest();
	Check(g_car.lr[tank] == 1.25f, "a NaN changes nothing");
	S_VehicleAim newer = Aim(80, 2.0f, 0.05f);
	newer.hdr.sendTimeMs = 5000;
	S_VehicleAim older = Aim(80, 1.5f, 0.05f);
	older.hdr.sendTimeMs = 4960;
	c.HandleMessage(Wrap(newer, CH_SNAPSHOT));
	c.HandleMessage(Wrap(older, CH_SNAPSHOT));
	c.CorrectCarExtrasForTest();
	Check(g_car.lr[tank] == 2.0f, "one that arrives behind a newer one is dropped");

	// Ours.
	Client d;
	d.SetBridge(CarBridge());
	d.HandleMessage(Wrap(Welcome(0)));
	g_car.localHandle = 9;
	g_car.hasGun[9]   = true;
	g_car.lr[9]       = 0.5f;
	g_car.ud[9]       = 0.05f;
	d.HandleMessage(Wrap(WeDrive(90, 122)));
	d.TickCarExtrasForTest();
	Check(d.AimReportsSentForTest() == 1, "the turret of the tank we drive goes out");
	d.TickCarExtrasForTest();
	Check(d.AimReportsSentForTest() == 1, "not again while it stands still");
	g_car.lr[9] = 0.6f;
	d.TickCarExtrasForTest();
	Check(d.AimReportsSentForTest() == 2, "again when it turns");
	d.CorrectCarExtrasForTest();
	Check(g_car.aimWrites == 0, "and nothing is ever written onto our own");

	g_car.hasGun[9] = false;
	d.TickCarExtrasForTest();
	Check(d.AimReportsSentForTest() == 2, "a car with no gun says nothing");
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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t bits = Dword(img, va);
	float          f    = 0.0f;
	std::memcpy(&f, &bits, sizeof f);
	return f;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> want) {
	uint32_t at = va;
	for (int b : want) {
		if (b >= 0 && Byte(img, at) != static_cast<uint8_t>(b))
			return false;
		++at;
	}
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;

// Every call in .text to `to`.
std::vector<uint32_t> CallersOf(const std::vector<uint8_t> &img, uint32_t to) {
	std::vector<uint32_t> out;
	for (uint32_t va = kTextBegin; va + 5 <= kTextEnd; ++va)
		if (CallsTo(img, va, to))
			out.push_back(va);
	return out;
}

constexpr uint32_t FindPlayerVehicle = 0x004A10C0;

void TestTheAlarmAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\na car's alarm against gta3.exe\n");
	// Every 16-bit access to [reg+1A0h] in .text: 66, one of the ALU/mov
	// opcodes, a mod-10 ModRM with no SIB, then the displacement.
	std::set<uint32_t> found;
	for (uint32_t va = kTextBegin; va + 7 <= kTextEnd; ++va) {
		if (Byte(img, va) != 0x66)
			continue;
		const uint8_t op = Byte(img, va + 1), modrm = Byte(img, va + 2);
		const bool    alu = op == 0x8B || op == 0x89 || op == 0x83 || op == 0xC7 ||
		                 op == 0x29 || op == 0x3B || op == 0x39 || op == 0x01 ||
		                 op == 0x2B || op == 0xFF || op == 0x81;
		if (!alu || (modrm & 0xC0) != 0x80 || (modrm & 7) == 4)
			continue;
		if (Dword(img, va + 3) == offs::VEH_ALARM_STATE)
			found.insert(va);
	}
	const std::set<uint32_t> known = {
	    0x004971BE, 0x004971C8, 0x004971D1, 0x004971DB,   // two cars touching
	    0x004A1D60, 0x004A1D67,                           // LoadVehiclePool
	    0x004C26C9, 0x0053C518,                           // the local player's car near the police
	    0x004CF3EF, 0x004CF3F9,                           // a driver seated
	    0x00538150,                                       // PreRender's flashing lights
	    0x00542884, 0x00542AE2,                           // a generator arming its car
	    0x00550EF0, 0x00551049,                           // the constructors
	    0x005525A4, 0x005525FA, 0x00552605,               // ProcessCarAlarm
	    0x0056C44F,                                       // the audio
	};
	Check(found == known, "nineteen word accesses to +0x1A0, and no writer but the known ones");

	Check(Bytes(img, ALARM_ARM_ON_SEATING, {0x66, 0x83, 0xBD, 0xA0, 0x01, 0x00, 0x00, 0xFF}) &&
	          Bytes(img, ALARM_ARM_ON_SEATING + 10,
	                {0x66, 0xC7, 0x85, 0xA0, 0x01, 0x00, 0x00, 0x98, 0x3A}),
	      "a driver seated in an armed car sets 15000 (0x004CF3EF)");
	Check(Bytes(img, ALARM_ARM_ON_COLLISION, {0x66, 0x83, 0xBD, 0xA0, 0x01, 0x00, 0x00, 0xFF}) &&
	          Bytes(img, ALARM_ARM_ON_COLLISION + 10,
	                {0x66, 0xC7, 0x85, 0xA0, 0x01, 0x00, 0x00, 0x98, 0x3A}),
	      "and so does an armed car touching another (0x004971BE)");
	Check(VEHICLE_ALARM_SET_MS == VEHICLE_ALARM_MS, "the wire's 15 s is the engine's");
	Check(Bytes(img, 0x00542884, {0x66, 0x83, 0x8B, 0xA0, 0x01, 0x00, 0x00, 0xFF}),
	      "a generator arms its car with -1");
	Check(Bytes(img, CVehicle__ProcessCarAlarm,
	            {0x53, 0x83, 0xEC, 0x10, 0x66, 0x8B, 0x91, 0xA0, 0x01, 0x00, 0x00}),
	      "ProcessCarAlarm reads the countdown first thing");
	const std::vector<uint32_t> ticks = CallersOf(img, CVehicle__ProcessCarAlarm);
	Check(ticks.size() == 2 && ticks[0] == 0x005316B2 && ticks[1] == 0x0053F2D2,
	      "and is called from the car's and the boat's ProcessControl, nowhere else");
	Check(Bytes(img, ALARM_READ_BY_AUDIO, {0x66, 0x8B, 0x95, 0xA0, 0x01, 0x00, 0x00}) &&
	          Bytes(img, ALARM_READ_BY_AUDIO + 0x10, {0x66, 0x83, 0xFA, 0xFF}),
	      "the audio sounds anything but 0 and -1");
	Check(Bytes(img, ALARM_READ_BY_PRERENDER, {0x66, 0x8B, 0x95, 0xA0, 0x01, 0x00, 0x00}),
	      "and PreRender flashes the lights off the same test");
}

void TestTheGunAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\nwhere a car's gun points, against gta3.exe\n");
	Check(Bytes(img, 0x00531FEE, {0x83, 0xF8, CAR_GUN_MODEL_FIRETRUCK}) &&
	          CallsTo(img, 0x00531FF7, CAutomobile__FireTruckControl),
	      "ProcessControl sends model 0x61 to FireTruckControl");
	Check(Bytes(img, 0x00532001, {0x83, 0xF8, CAR_GUN_MODEL_RHINO}) &&
	          CallsTo(img, 0x0053200A, CAutomobile__TankControl),
	      "and 0x7A to TankControl");
	Check(MODEL_RHINO == CAR_GUN_MODEL_RHINO, "the same tank in both files");
	Check(CallsTo(img, 0x0053D5E5, FindPlayerVehicle) &&
	          Bytes(img, 0x0053D5EA, {0x39, 0xC3, 0x0F, 0x85}),
	      "TankControl leaves at once for any car but the local player's");
	Check(CallsTo(img, 0x0052259A, FindPlayerVehicle) &&
	          Bytes(img, 0x0052259F, {0x39, 0xC3, 0x0F, 0x85}),
	      "and so does FireTruckControl's pad half");
	Check(Bytes(img, 0x0053D646, {0xD8, 0xAB, 0x80, 0x05, 0x00, 0x00}) &&
	          Bytes(img, 0x0053D64C, {0xD9, 0x9B, 0x80, 0x05, 0x00, 0x00}),
	      "the tank turns m_fCarGunLR at +0x580");
	Check(Float(img, 0x00600810) == 0.00015f, "at 0.00015 a step per unit of stick");
	Check(Bytes(img, 0x005225F0, {0xD8, 0x83, 0x80, 0x05, 0x00, 0x00}) &&
	          Bytes(img, 0x0052262A, {0xD8, 0x83, 0x84, 0x05, 0x00, 0x00}),
	      "the fire truck turns +0x580 and tilts +0x584");
	Check(Bytes(img, 0x0052CF82, {0xC7, 0x80, 0x80, 0x05, 0x00, 0x00, 0, 0, 0, 0}) &&
	          Bytes(img, 0x0052CF90, {0xC7, 0x80, 0x84, 0x05, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3D}),
	      "the constructor starts them at 0 and 0.05");
	Check(offs::AUTO_GUN_LR == 0x580 && offs::AUTO_GUN_UD == 0x584, "which is what we write");
	Check(Dword(img, CAutomobile__vtable_Render) == CAutomobile__Render &&
	          CAutomobile__vtable_Render == CAutomobile__vtable + 13 * 4,
	      "CAutomobile's vtable slot 13 is Render");
	Check(Bytes(img, 0x00539ED0, {0x83, 0xF8, 0x7A}) &&
	          Bytes(img, TANK_TURRET_READ_BY_RENDER, {0xFF, 0xB3, 0x80, 0x05, 0x00, 0x00}),
	      "which turns every tank's turret by +0x580, whoever drives it");
}

void TestTheFlagsAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\nthe taxi light and the handbrake, against gta3.exe\n");
	Check(Bytes(img, CAutomobile__SetTaxiLight,
	            {0x8A, 0x44, 0x24, 0x04, 0x8A, 0x91, 0xD9, 0x04, 0x00, 0x00, 0x24, 0x01, 0x80,
	             0xE2, 0xF7, 0xC0, 0xE0, 0x03}),
	      "SetTaxiLight is bit 3 of +0x4D9");
	const std::vector<uint32_t> set = CallersOf(img, CAutomobile__SetTaxiLight);
	Check(set.size() == 1 && set[0] == 0x00443424, "called from SET_TAXI_LIGHTS alone");
	Check(Bytes(img, TAXI_LIGHT_READ_BY_PRERENDER,
	            {0x8A, 0x85, 0xD9, 0x04, 0x00, 0x00, 0xC0, 0xE8, 0x03, 0x24, 0x01}),
	      "and PreRender draws the corona off it");
	Check(Bytes(img, 0x0052C78E, {0x80, 0x3D, 0x21, 0xCD, 0x95, 0x00, 0x00}) &&
	          Bytes(img, 0x0052C7A1, {0x24, 0xF7, 0x0C, 0x08}) &&
	          Bytes(img, 0x0052C7BA, {0x24, 0xF7}),
	      "a new car takes m_sAllTaxiLights");
	// Every mention of m_sAllTaxiLights in .text: the constructor's read and
	// SetAllTaxiLights' write.
	std::vector<uint32_t> mentions;
	for (uint32_t va = kTextBegin; va + 4 <= kTextEnd; ++va)
		if (Dword(img, va) == CAutomobile__ms_sAllTaxiLights)
			mentions.push_back(va);
	Check(mentions.size() == 2 && mentions[0] == 0x0052C790 && mentions[1] == 0x0053C445 &&
	          Bytes(img, CAutomobile__SetAllTaxiLights, {0x8A, 0x44, 0x24, 0x04, 0xA2}),
	      "the global has one writer, SetAllTaxiLights");
	const std::vector<uint32_t> all = CallersOf(img, CAutomobile__SetAllTaxiLights);
	Check(all.size() == 1 && all[0] == 0x004437E8,
	      "called from one script instruction alone, so a new car's light is off unless a "
	      "script says otherwise");
	Check(Bytes(img, 0x0053BE94, {0x8A, 0x83, 0xD9, 0x04, 0x00, 0x00, 0x24, 0xF7}),
	      "and BlowUpCar puts it out");
	Check(Bytes(img, HANDBRAKE_WRITTEN_FROM_PAD,
	            {0x8A, 0x85, 0xF5, 0x01, 0x00, 0x00, 0xC0, 0xE3, 0x05, 0x24, 0xDF, 0x08, 0xD8,
	             0x88, 0x85, 0xF5, 0x01, 0x00, 0x00}),
	      "the player's handbrake is bit 5 of +0x1F5, from his pad");
}

void TestTheFastWailAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\nthe fast wail, against gta3.exe\n");
	Check(Bytes(img, 0x0056C226, {0x80, 0xBD, 0x2E, 0x02, 0x00, 0x00, 0x00, 0x74}) &&
	          CallsTo(img, 0x0056C235, 0x0056C3F0) && Bytes(img, 0x0056C23C, {0x74}),
	      "ProcessVehicleHorn plays no horn on a switching car with its siren on");
	Check(Bytes(img, 0x0056C4D4, {0x80, 0xBD, 0x2C, 0x02, 0x00, 0x00, 0x00, 0x74}) &&
	          Bytes(img, 0x0056C4DD, {0x83, 0x7B, 0x10, 0x07}),
	      "and the siren takes the timer as the fast wail, but on the fire truck");
	Check(Bytes(img, 0x0056C254, {0x83, 0xF8, 0x71}), "Mr Whoopee plays no horn at all");
}

void TestAgainstTheImage() {
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("\n  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check a car's "
		            "alarm, gun and lights against one\n");
		return;
	}
	std::printf("\n  reading %s\n", from.c_str());
	TestTheAlarmAgainstTheImage(img);
	TestTheGunAgainstTheImage(img);
	TestTheFlagsAgainstTheImage(img);
	TestTheFastWailAgainstTheImage(img);
}

} // namespace

int RunCarExtrasTests() {
	std::printf("\n");
	TestWhatIsLeftOfAnAlarm();
	TestWhenTheAlarmIsNews();
	TestWhenTheGunIsNews();
	TestThePoliceCarsFastWailGoesOut();
	TestACopyGoesOffWithItsOwner();
	TestOurAlarmIsSaidOnce();
	TestTakingTheWheelSaysNothing();
	TestTheTurretFollowsItsDriver();
	TestAgainstTheImage();
	return g_carExtrasFailures;
}
